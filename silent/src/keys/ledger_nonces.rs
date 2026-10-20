//! Ledger protocol adapter for the shared MuSig session cache.

pub use crate::keys::musig_sessions::{
    can_publish, check_round2, count, descriptor, ensure_snapshot, finish, has_cache,
    invalidate_signing, prepare, refresh, requested_by, restart, signing_status, start_attempt,
    SessionSource, Signing, SigningError,
};

use std::{collections::BTreeSet, path::Path};

use bitcoin::{
    absolute::LockTime, bip32::Fingerprint, secp256k1::PublicKey, transaction::Version, Amount,
    OutPoint, Psbt, ScriptBuf, Sequence, Transaction, TxIn, TxOut, Witness,
};
use bwk::{
    bwk_descriptor::{
        updater::update_input,
        wallet::{WalletDefiniteDescriptor, WalletDescriptor, WalletDescriptorExt},
    },
    bwk_keys::musig::MusigKey,
    bwk_sign::{
        miniscript,
        musig::{musig_status, own_slots, slot_infos, slot_sighash, MusigRound},
    },
    bwk_tx::bwk_psbt::musig::musig_input_pub_nonces,
};

use crate::{
    config::datadir,
    keys::{
        musig_sessions::{
            binding, session_key, ExpressionSlot, LockedCache, Nonce, Session, MAX_INPUT,
        },
        psbt,
        record::RouteKind,
    },
};

pub(crate) const FUNDING_SATS: u64 = 100_000;
pub(crate) const FETCH_FEE_SATS: u64 = 1_000;
pub const MAX_LEDGER_NONCE_SESSIONS: usize = crate::keys::musig_sessions::MAX_SESSIONS;

pub fn is_ledger(kind: &str) -> bool {
    matches!(kind, "ledger" | "ledger-simulator")
}

pub(crate) fn owned_expressions(
    policy: &WalletDescriptor,
    fingerprint: Fingerprint,
) -> Result<Vec<usize>, String> {
    let miniscript::Descriptor::Tr(tr) = policy else {
        return Err("Ledger nonce caching requires a taproot policy".into());
    };
    let keys = std::iter::once(tr.internal_key().clone())
        .chain(tr.iter_scripts().flat_map(|(_, ms)| ms.iter_pk()));
    let mut owned = Vec::new();
    for (index, key) in keys.enumerate() {
        let MusigKey::Musig(aggregate) = key else {
            continue;
        };
        match aggregate
            .cosigners()
            .iter()
            .filter(|key| key.master_fingerprint() == fingerprint)
            .count()
        {
            0 => {}
            1 => owned.push(index),
            _ => {
                return Err(
                    "Ledger cannot sign multiple owned participants in one MuSig expression".into(),
                )
            }
        }
    }
    if owned.is_empty() {
        return Err("policy has no MuSig expression owned by this Ledger".into());
    }
    Ok(owned)
}

pub(crate) fn expression_slots(
    definite: &WalletDefiniteDescriptor,
    input: usize,
    owned: &[usize],
    fingerprint: Fingerprint,
) -> Result<Vec<ExpressionSlot>, String> {
    Ok(bwk::bwk_sign::musig::expression_slots(input, definite)
        .map_err(|e| e.to_string())?
        .into_iter()
        .filter(|info| info.signer.0 == fingerprint && owned.contains(&info.expression_index))
        .map(|info| ExpressionSlot {
            expression: info.expression_index,
            slot: info.slot,
        })
        .collect())
}

pub(crate) fn synthetic(
    policy: &WalletDescriptor,
    fingerprint: Fingerprint,
) -> Result<Psbt, String> {
    let owned = owned_expressions(policy, fingerprint)?;
    let definite = policy
        .single_paths()
        .map_err(|e| e.to_string())?
        .first()
        .ok_or("no descriptor path")?
        .derive_at(0)
        .map_err(|e| e.to_string())?;
    let spent = TxOut {
        value: Amount::from_sat(FUNDING_SATS),
        script_pubkey: definite.script_pubkey(),
    };
    let funding = Transaction {
        version: Version::TWO,
        lock_time: LockTime::ZERO,
        input: vec![TxIn {
            previous_output: OutPoint::null(),
            script_sig: ScriptBuf::from_bytes(vec![1, 1]),
            sequence: Sequence::MAX,
            witness: Witness::new(),
        }],
        output: vec![spent.clone(); MAX_INPUT],
    };
    let tx = Transaction {
        version: Version::TWO,
        lock_time: LockTime::ZERO,
        input: (0..MAX_INPUT)
            .map(|index| TxIn {
                previous_output: OutPoint::new(funding.compute_txid(), index as u32),
                script_sig: ScriptBuf::new(),
                sequence: Sequence::MAX,
                witness: Witness::new(),
            })
            .collect(),
        output: vec![TxOut {
            value: Amount::from_sat(FUNDING_SATS * MAX_INPUT as u64 - FETCH_FEE_SATS),
            script_pubkey: definite.script_pubkey(),
        }],
    };
    let mut psbt = Psbt::from_unsigned_tx(tx).map_err(|e| e.to_string())?;
    let participants: BTreeSet<_> = expression_slots(&definite, 0, &owned, fingerprint)?
        .iter()
        .map(|entry| entry.slot.participant.x_only_public_key().0)
        .collect();
    for index in 0..MAX_INPUT {
        psbt.inputs[index].witness_utxo = Some(spent.clone());
        psbt.inputs[index].non_witness_utxo = Some(funding.clone());
        update_input(&mut psbt, index, &definite).map_err(|e| e.to_string())?;
        // Leave synthetic aggregate origins intact, but do not request ordinary signatures.
        psbt.inputs[index]
            .tap_key_origins
            .retain(|key, (_, (fp, _))| *fp != fingerprint || participants.contains(key));
        psbt.inputs[index].bip32_derivation.clear();
    }
    Ok(psbt)
}

pub(crate) fn requested_slots(
    real: &Psbt,
    policy: &WalletDescriptor,
    fingerprint: Fingerprint,
) -> Result<Vec<ExpressionSlot>, String> {
    if real.inputs.len() > MAX_INPUT {
        return Err("Ledger nonce cache supports at most 8 inputs".into());
    }
    let owned = owned_expressions(policy, fingerprint)?;
    let requested = own_slots(real, policy, fingerprint).map_err(|e| e.to_string())?;
    if requested.is_empty() {
        return Err("transaction requests no owned MuSig slots".into());
    }
    let singles = policy.single_paths().map_err(|e| e.to_string())?;
    let infos = slot_infos(real, policy).map_err(|e| e.to_string())?;
    let mut result = Vec::new();
    for slot in requested {
        slot_sighash(real, &slot).map_err(|e| e.to_string())?;
        let matches: Vec<_> = infos
            .iter()
            .filter(|info| info.slot == slot && info.signer.0 == fingerprint)
            .collect();
        let [info] = matches.as_slice() else {
            return Err("unsupported or ambiguous Ledger MuSig expression coverage".into());
        };
        result.push(ExpressionSlot {
            expression: info.expression_index,
            slot,
        });
    }
    let template = singles
        .first()
        .ok_or("no descriptor path")?
        .derive_at(0)
        .map_err(|e| e.to_string())?;
    let cached = expression_slots(&template, 0, &owned, fingerprint)?;
    for entry in &result {
        if !cached.iter().any(|template| {
            template.expression == entry.expression
                && template.slot.participant == entry.slot.participant
        }) {
            return Err(
                "Ledger cached sessions do not cover address-derived MuSig participants".into(),
            );
        }
    }
    Ok(result)
}

pub struct Fetch {
    pub(crate) store: LockedCache,
    policy_text: String,
    pub(crate) policy: WalletDescriptor,
    pub(crate) owned: Vec<usize>,
    template: Psbt,
    pub(crate) pending: Session,
    pub(crate) remaining: usize,
    completed: Vec<Session>,
}

impl Fetch {
    pub fn begin(
        fingerprint: Fingerprint,
        policy_text: &str,
        quantity: usize,
    ) -> Result<Self, String> {
        Self::begin_at(
            &datadir().join("musig-sessions"),
            fingerprint,
            policy_text,
            quantity,
        )
    }

    pub(crate) fn begin_at(
        root: &Path,
        fingerprint: Fingerprint,
        policy_text: &str,
        quantity: usize,
    ) -> Result<Self, String> {
        Self::begin_locked(
            LockedCache::open(root, fingerprint)?,
            policy_text,
            quantity,
            None,
        )
    }

    pub fn for_signing(
        fingerprint: Fingerprint,
        policy_text: &str,
        bytes: &[u8],
    ) -> Result<Option<Self>, String> {
        Self::for_signing_at(
            &datadir().join("musig-sessions"),
            fingerprint,
            policy_text,
            bytes,
        )
    }

    pub(crate) fn for_signing_at(
        root: &Path,
        fingerprint: Fingerprint,
        policy_text: &str,
        bytes: &[u8],
    ) -> Result<Option<Self>, String> {
        let real = psbt::parse_v2(bytes)
            .map_err(|e| e.to_string())?
            .into_bitcoin_psbt()
            .map_err(|e| e.to_string())?;
        let policy = descriptor(policy_text)?;
        requested_slots(&real, &policy, fingerprint)?;
        let binding = binding(&real, &policy, fingerprint)?;
        let store = LockedCache::open(root, fingerprint)?;
        if store
            .state
            .available
            .iter()
            .any(|session| session.policy == policy_text && session.provider == RouteKind::Usb)
            || store.state.allocated.iter().any(|allocation| {
                allocation.binding == binding && allocation.provider == RouteKind::Usb
            })
            || store.state.is_retired(binding, RouteKind::Usb)
            || musig_status(&real, &policy)
                .map_err(|e| e.to_string())?
                .round
                != MusigRound::Nonces
        {
            return Ok(None);
        }
        Self::begin_locked(store, policy_text, 1, Some(binding)).map(Some)
    }

    fn begin_locked(
        mut store: LockedCache,
        policy_text: &str,
        quantity: usize,
        fetched_for: Option<[u8; 32]>,
    ) -> Result<Self, String> {
        if !(1..=MAX_LEDGER_NONCE_SESSIONS).contains(&quantity) {
            return Err("nonce quantity must be between 1 and 8".into());
        }
        let policy = descriptor(policy_text)?;
        let owned = owned_expressions(&policy, store.state.fingerprint)?;
        let template = synthetic(&policy, store.state.fingerprint)?;
        let pending = Self::reserve(&mut store, policy_text, fetched_for)?;
        Ok(Self {
            store,
            policy_text: policy_text.to_owned(),
            policy,
            owned,
            template,
            pending,
            remaining: quantity,
            completed: Vec::new(),
        })
    }

    fn reserve(
        store: &mut LockedCache,
        policy: &str,
        fetched_for: Option<[u8; 32]>,
    ) -> Result<Session, String> {
        let mut id = [0; 32];
        getrandom::getrandom(&mut id).map_err(|e| e.to_string())?;
        Ok(Session {
            provider: RouteKind::Usb,
            secret: None,
            id,
            order: store.reserve_order()?,
            policy: policy.to_owned(),
            nonces: Vec::new(),
            fetched_for,
        })
    }

    pub fn payload(&self) -> Psbt {
        let mut psbt = self.template.clone();
        psbt.proprietary
            .insert(session_key(), self.pending.id.to_vec());
        psbt
    }

    pub fn accept(&mut self, signed: &Psbt) -> Result<bool, String> {
        if self.remaining == 0 {
            return Err("Ledger nonce fetch is already complete".into());
        }
        if signed.unsigned_tx != self.template.unsigned_tx || signed.inputs.len() != MAX_INPUT {
            return Err("Ledger returned a different nonce transaction".into());
        }
        self.pending.nonces = self.read_nonces(signed)?;
        self.completed.push(self.pending.clone());
        self.remaining -= 1;
        if self.remaining == 0 {
            let mut next = self.store.state.clone();
            for session in &self.completed {
                next.replace(session.clone());
            }
            self.store.save(next)?;
            return Ok(true);
        }
        self.pending = Self::reserve(&mut self.store, &self.policy_text, self.pending.fetched_for)?;
        Ok(false)
    }

    fn read_nonces(&self, signed: &Psbt) -> Result<Vec<Nonce>, String> {
        let definite = self
            .policy
            .single_paths()
            .map_err(|e| e.to_string())?
            .first()
            .ok_or("no descriptor path")?
            .derive_at(0)
            .map_err(|e| e.to_string())?;
        let mut nonces = Vec::new();
        for input in 0..MAX_INPUT {
            let fields =
                musig_input_pub_nonces(&signed.inputs[input]).map_err(|e| e.to_string())?;
            let expected =
                expression_slots(&definite, input, &self.owned, self.store.state.fingerprint)?;
            let unique: BTreeSet<_> = expected.iter().map(|entry| entry.slot).collect();
            if unique.len() != expected.len() {
                return Err(
                    "Ledger nonce yields cannot distinguish repeated identical expressions".into(),
                );
            }
            for entry in expected {
                let matches: Vec<_> = fields
                    .iter()
                    .filter(|field| {
                        field.participant == entry.slot.participant
                            && field.aggregate == entry.slot.aggregate
                            && field.leaf_hash == entry.slot.leaf_hash
                    })
                    .collect();
                let [field] = matches.as_slice() else {
                    return Err("Ledger did not return complete, unambiguous nonce coverage".into());
                };
                PublicKey::from_slice(&field.nonce[..33]).map_err(|e| e.to_string())?;
                PublicKey::from_slice(&field.nonce[33..]).map_err(|e| e.to_string())?;
                nonces.push(Nonce {
                    input,
                    expression: entry.expression,
                    participant: field.participant,
                    bytes: field.nonce.to_vec(),
                });
            }
        }
        Ok(nonces)
    }
}
