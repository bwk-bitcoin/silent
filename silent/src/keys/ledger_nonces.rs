//! Ledger caller sessions, shared by every account and network of a fingerprint.

use std::{
    collections::{BTreeMap, BTreeSet},
    fs::{self, File, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    str::FromStr,
    sync::{LazyLock, Mutex},
};

use bitcoin::{
    absolute::LockTime,
    bip32::{ChildNumber, Fingerprint},
    hashes::{sha256, Hash, HashEngine},
    psbt::raw::ProprietaryKey,
    secp256k1::{PublicKey, Secp256k1},
    taproot::{LeafVersion, TapLeafHash, TapNodeHash},
    transaction::Version,
    Amount, OutPoint, Psbt, Sequence, Transaction, TxIn, TxOut, Witness,
};
use bwk::{
    bwk_descriptor::{
        descriptor::Descriptor,
        updater::update_input,
        wallet::{WalletDefiniteDescriptor, WalletDescriptor, WalletDescriptorExt},
    },
    bwk_keys::musig::{DefiniteMusigAggregate, DefiniteMusigKey, MusigKey},
    bwk_sign::{
        miniscript,
        musig::{
            musig_status, own_slots, session_id, slot_sighash, MusigRound, MusigSlot, MusigStatus,
        },
    },
    bwk_tx::bwk_psbt::musig::{
        musig_input_partial_sig, musig_input_participants_all, musig_input_pub_nonce,
        musig_input_pub_nonces, set_musig_input_pub_nonce, MusigPubNonce,
    },
};
use fs2::FileExt;
use serde::{Deserialize, Serialize};

use crate::{config::datadir, keys::psbt, state_crypt};

pub const MAX_INPUT: usize = 8;
pub const MAX_LEDGER_NONCE_SESSIONS: usize = 8;
const FUNDING_SATS: u64 = 100_000;
const FETCH_FEE_SATS: u64 = 1_000;
const CACHE_VERSION: u32 = 1;

static SNAPSHOTS: LazyLock<Mutex<BTreeMap<Fingerprint, Cache>>> =
    LazyLock::new(|| Mutex::new(BTreeMap::new()));

pub fn is_ledger(kind: &str) -> bool {
    matches!(kind, "ledger" | "ledger-simulator")
}

pub fn descriptor(policy: &str) -> Result<WalletDescriptor, String> {
    Descriptor::from_str(policy)
        .map_err(|e| e.to_string())?
        .into_miniscript()
        .map_err(|e| e.to_string())
}

pub fn count(fingerprint: Fingerprint, policy: &str) -> usize {
    SNAPSHOTS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .get(&fingerprint)
        .map(|state| {
            state
                .available
                .iter()
                .filter(|session| session.policy == policy)
                .count()
        })
        .unwrap_or(0)
}

fn publish_snapshot(fingerprint: Fingerprint, state: &Cache) {
    SNAPSHOTS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .insert(fingerprint, state.clone());
}

pub fn refresh(fingerprint: Fingerprint) -> Result<(), String> {
    refresh_at(&datadir().join("ledger-nonces"), fingerprint)
}

fn refresh_at(root: &Path, fingerprint: Fingerprint) -> Result<(), String> {
    let store = LockedCache::open(root, fingerprint)?;
    publish_snapshot(fingerprint, &store.state);
    Ok(())
}

pub fn ensure_snapshot(fingerprint: Fingerprint) -> Result<(), String> {
    if SNAPSHOTS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .contains_key(&fingerprint)
    {
        return Ok(());
    }
    refresh(fingerprint)
}

#[derive(Clone, Serialize, Deserialize)]
struct Nonce {
    input: usize,
    expression: usize,
    participant: PublicKey,
    bytes: Vec<u8>,
}

#[derive(Clone, Serialize, Deserialize)]
struct Session {
    id: [u8; 32],
    order: u64,
    policy: String,
    nonces: Vec<Nonce>,
}

#[derive(Clone, Serialize, Deserialize)]
struct Allocation {
    binding: [u8; 32],
    id: [u8; 32],
    // The complete published slot set is fixed, even if a later PSBT asks for more leaves.
    slots: Vec<PublishedNonce>,
    round2_nonces: Option<[u8; 32]>,
    state: AllocationState,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
enum AllocationState {
    Active,
    Consumed,
    Invalid,
}

#[derive(Debug, thiserror::Error)]
pub enum SigningError {
    #[error("{0}")]
    Invalid(String),
    #[error("{0}")]
    SessionLost(String),
}

impl From<String> for SigningError {
    fn from(error: String) -> Self {
        Self::Invalid(error)
    }
}

impl From<&str> for SigningError {
    fn from(error: &str) -> Self {
        Self::Invalid(error.to_owned())
    }
}

impl Allocation {
    fn compatible_nonces(&self, real: &Psbt, require_all: bool) -> Result<bool, String> {
        for published in &self.slots {
            let Some(input) = real.inputs.get(published.input) else {
                return Ok(false);
            };
            let existing = musig_input_pub_nonce(
                input,
                published.participant,
                published.aggregate,
                published.leaf_hash,
            )
            .map_err(|e| e.to_string())?;
            match existing {
                Some(nonce) if nonce.as_slice() != published.bytes.as_slice() => return Ok(false),
                None if require_all => return Ok(false),
                _ => {}
            }
        }
        Ok(true)
    }
}

#[derive(Clone, Serialize, Deserialize)]
struct PublishedNonce {
    input: usize,
    participant: PublicKey,
    aggregate: PublicKey,
    leaf_hash: Option<TapLeafHash>,
    bytes: Vec<u8>,
}

impl PublishedNonce {
    fn slot(&self) -> MusigSlot {
        MusigSlot {
            input_index: self.input,
            participant: self.participant,
            aggregate: self.aggregate,
            leaf_hash: self.leaf_hash,
        }
    }

    fn field(&self) -> Result<MusigPubNonce, String> {
        Ok(MusigPubNonce {
            participant: self.participant,
            aggregate: self.aggregate,
            leaf_hash: self.leaf_hash,
            nonce: self
                .bytes
                .as_slice()
                .try_into()
                .map_err(|_| "invalid cached public nonce length")?,
        })
    }
}

#[derive(Clone, Serialize, Deserialize)]
struct Cache {
    version: u32,
    fingerprint: Fingerprint,
    fetch_order: u64,
    available: Vec<Session>,
    allocated: Vec<Allocation>,
    // A restart also fences old requests that have not reached allocation yet.
    retired: BTreeSet<[u8; 32]>,
}

struct LockedCache {
    _lock: File,
    path: PathBuf,
    state: Cache,
}

fn cache_path(root: &Path, fingerprint: Fingerprint) -> Result<PathBuf, String> {
    let stem = state_crypt::opaque_name("ledger-nonces", &fingerprint.to_string())?;
    Ok(root.join(format!("{stem}.{}", state_crypt::SEALED_EXTENSION)))
}

pub fn has_cache(fingerprint: Fingerprint) -> Result<bool, String> {
    cache_path(&datadir().join("ledger-nonces"), fingerprint)?
        .try_exists()
        .map_err(|e| e.to_string())
}

impl LockedCache {
    fn open(root: &Path, fingerprint: Fingerprint) -> Result<Self, String> {
        #[cfg(unix)]
        {
            use std::os::unix::fs::DirBuilderExt;
            fs::DirBuilder::new()
                .recursive(true)
                .mode(0o700)
                .create(root)
                .map_err(|e| e.to_string())?;
        }
        #[cfg(not(unix))]
        fs::create_dir_all(root).map_err(|e| e.to_string())?;
        #[cfg(unix)]
        File::open(root.parent().ok_or("cache directory has no parent")?)
            .and_then(|dir| dir.sync_all())
            .map_err(|e| e.to_string())?;
        let path = cache_path(root, fingerprint)?;
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(path.with_extension("lock"))
            .map_err(|e| e.to_string())?;
        lock.try_lock_exclusive()
            .map_err(|e| format!("Ledger nonce cache is busy: {e}"))?;
        let state: Cache = match state_crypt::read_state(&path)? {
            Some(json) => serde_json::from_str(&json).map_err(|e| e.to_string())?,
            None => Cache {
                version: CACHE_VERSION,
                fingerprint,
                fetch_order: 0,
                available: Vec::new(),
                allocated: Vec::new(),
                retired: BTreeSet::new(),
            },
        };
        if state.version != CACHE_VERSION || state.fingerprint != fingerprint {
            return Err("unsupported or mismatched Ledger nonce cache".into());
        }
        Ok(Self {
            _lock: lock,
            path,
            state,
        })
    }

    fn save(&mut self, next: Cache) -> Result<(), String> {
        let sealed = state_crypt::seal(&serde_json::to_string(&next).map_err(|e| e.to_string())?)?;
        let temp = self.path.with_extension("tmp");
        let mut options = OpenOptions::new();
        options.create(true).truncate(true).write(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let mut file = options.open(&temp).map_err(|e| e.to_string())?;
        file.write_all(&sealed).map_err(|e| e.to_string())?;
        file.sync_all().map_err(|e| e.to_string())?;
        fs::rename(&temp, &self.path).map_err(|e| e.to_string())?;
        // The rename must survive power loss before a nonce can leave this process.
        #[cfg(unix)]
        File::open(self.path.parent().ok_or("cache has no parent")?)
            .and_then(|dir| dir.sync_all())
            .map_err(|e| e.to_string())?;
        self.state = next;
        publish_snapshot(self.state.fingerprint, &self.state);
        Ok(())
    }

    fn invalidate(&mut self) -> Result<(), String> {
        let mut next = self.state.clone();
        next.available.clear();
        self.save(next)
    }

    fn reserve_order(&mut self) -> Result<u64, String> {
        let mut next = self.state.clone();
        next.fetch_order = next
            .fetch_order
            .checked_add(1)
            .ok_or("fetch order exhausted")?;
        let order = next.fetch_order;
        self.save(next)?;
        Ok(order)
    }

    fn allocate(
        &mut self,
        binding: [u8; 32],
        policy: &str,
        slots: &[ExpressionSlot],
        round1: bool,
    ) -> Result<Option<Allocation>, SigningError> {
        if self.state.retired.contains(&binding) {
            return Err(SigningError::SessionLost(
                "Ledger signing attempt was restarted".into(),
            ));
        }
        if let Some(allocation) = self
            .state
            .allocated
            .iter()
            .find(|entry| entry.binding == binding)
        {
            if allocation.state != AllocationState::Active {
                return Err(SigningError::SessionLost(
                    "Ledger signing attempt is no longer active".into(),
                ));
            }
            return Ok(Some(allocation.clone()));
        }
        if !round1 {
            return Err(SigningError::SessionLost(
                "Ledger nonce allocation is missing; restart MuSig signing".into(),
            ));
        }
        let Some(index) = self
            .state
            .available
            .iter()
            .enumerate()
            .filter(|(_, session)| session.policy == policy)
            .min_by_key(|(_, session)| session.order)
            .map(|(index, _)| index)
        else {
            return Ok(None);
        };
        let mut next = self.state.clone();
        let session = next.available.remove(index);
        let published = session.published(slots)?;
        let allocation = Allocation {
            binding,
            id: session.id,
            slots: published,
            round2_nonces: None,
            state: AllocationState::Active,
        };
        next.allocated.push(allocation.clone());
        self.save(next)?;
        Ok(Some(allocation))
    }

    fn bind_round2(&mut self, allocation: &Allocation, real: &Psbt) -> Result<(), SigningError> {
        let transcript = nonce_transcript(real)?;
        match allocation.round2_nonces {
            Some(previous) if previous != transcript => {
                self.retire(allocation.binding, AllocationState::Invalid)?;
                Err(SigningError::SessionLost(
                    "MuSig nonces changed after Ledger round two started".into(),
                ))
            }
            Some(_) => Ok(()),
            None => self
                .save_round2(allocation.binding, transcript)
                .map_err(SigningError::from),
        }
    }

    fn save_round2(&mut self, binding: [u8; 32], transcript: [u8; 32]) -> Result<(), String> {
        let mut next = self.state.clone();
        let saved = next
            .allocated
            .iter_mut()
            .find(|entry| entry.binding == binding)
            .ok_or("allocation disappeared")?;
        saved.round2_nonces = Some(transcript);
        self.save(next)
    }

    fn retire(&mut self, binding: [u8; 32], state: AllocationState) -> Result<(), String> {
        let mut next = self.state.clone();
        match next
            .allocated
            .iter_mut()
            .find(|entry| entry.binding == binding)
        {
            Some(allocation) if allocation.state == AllocationState::Active => {
                allocation.state = state
            }
            Some(_) => return Ok(()),
            None => {
                next.retired.insert(binding);
            }
        }
        self.save(next)
    }
}

impl Session {
    fn published(&self, slots: &[ExpressionSlot]) -> Result<Vec<PublishedNonce>, String> {
        slots
            .iter()
            .map(|entry| {
                let nonce = self
                    .nonces
                    .iter()
                    .find(|nonce| {
                        nonce.input == entry.slot.input_index
                            && nonce.expression == entry.expression
                            && nonce.participant == entry.slot.participant
                    })
                    .ok_or("cached session does not cover this input's MuSig participant")?;
                Ok(PublishedNonce {
                    input: entry.slot.input_index,
                    participant: entry.slot.participant,
                    aggregate: entry.slot.aggregate,
                    leaf_hash: entry.slot.leaf_hash,
                    bytes: nonce.bytes.clone(),
                })
            })
            .collect()
    }
}

// Ledger numbers occurrences before filtering for MuSig or ownership.
pub fn owned_expressions(
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
        let count = aggregate
            .cosigners()
            .iter()
            .filter(|key| key.master_fingerprint() == fingerprint)
            .count();
        match count {
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

struct ExpressionSlot {
    expression: usize,
    slot: MusigSlot,
}

fn aggregate_slot(
    input: usize,
    expression: usize,
    aggregate: &DefiniteMusigAggregate,
    leaf_hash: Option<TapLeafHash>,
    root: Option<TapNodeHash>,
    fingerprint: Fingerprint,
) -> Result<ExpressionSlot, String> {
    let cosigner = aggregate
        .cosigners()
        .iter()
        .find(|key| key.master_fingerprint() == fingerprint)
        .ok_or("owned cosigner missing")?;
    let participant = cosigner
        .derive_public_key(&Secp256k1::verification_only())
        .map_err(|e| e.to_string())?
        .inner;
    let key = match leaf_hash {
        None => PublicKey::from_slice(
            &aggregate
                .key_path_key_agg(root)
                .map_err(|e| e.to_string())?
                .output_key(),
        )
        .map_err(|e| e.to_string())?,
        Some(_) => aggregate.key(),
    };
    Ok(ExpressionSlot {
        expression,
        slot: MusigSlot {
            input_index: input,
            participant,
            aggregate: key,
            leaf_hash,
        },
    })
}

fn expression_slots(
    definite: &WalletDefiniteDescriptor,
    input: usize,
    owned: &[usize],
    fingerprint: Fingerprint,
) -> Result<Vec<ExpressionSlot>, String> {
    let miniscript::Descriptor::Tr(tr) = definite else {
        return Err("not taproot".into());
    };
    let root = tr.spend_info().merkle_root();
    let keys = std::iter::once((tr.internal_key().clone(), None)).chain(
        tr.iter_scripts().flat_map(|(_, ms)| {
            let leaf = TapLeafHash::from_script(&ms.encode(), LeafVersion::TapScript);
            ms.iter_pk().map(move |key| (key, Some(leaf)))
        }),
    );
    let mut slots = Vec::new();
    for (expression, (key, leaf)) in keys.enumerate() {
        if !owned.contains(&expression) {
            continue;
        }
        let DefiniteMusigKey::Musig(aggregate) = key else {
            return Err("expression changed kind".into());
        };
        slots.push(aggregate_slot(
            input,
            expression,
            &aggregate,
            leaf,
            root,
            fingerprint,
        )?);
    }
    Ok(slots)
}

fn synthetic(policy: &WalletDescriptor, fingerprint: Fingerprint) -> Result<Psbt, String> {
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
            script_sig: bitcoin::ScriptBuf::from_bytes(vec![1, 1]),
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
                script_sig: bitcoin::ScriptBuf::new(),
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

fn session_key() -> ProprietaryKey {
    ProprietaryKey {
        prefix: b"MUSIG2_SESSION_ID".to_vec(),
        subtype: 0,
        key: Vec::new(),
    }
}

fn attempt_key() -> ProprietaryKey {
    ProprietaryKey {
        prefix: b"SILENT_MUSIG_ATTEMPT".to_vec(),
        subtype: 0,
        key: Vec::new(),
    }
}

pub fn start_attempt(psbt: &mut Psbt) -> Result<(), String> {
    let mut attempt = [0; 32];
    getrandom::getrandom(&mut attempt).map_err(|e| e.to_string())?;
    psbt.proprietary.insert(attempt_key(), attempt.to_vec());
    Ok(())
}

fn binding(
    real: &Psbt,
    policy: &WalletDescriptor,
    fingerprint: Fingerprint,
) -> Result<[u8; 32], String> {
    let transaction = session_id(real, policy, fingerprint)
        .map_err(|e| e.to_string())?
        .0;
    let Some(attempt) = real.proprietary.get(&attempt_key()) else {
        return Ok(transaction);
    };
    if attempt.len() != 32 {
        return Err("invalid MuSig signing attempt ID".into());
    }
    let mut engine = sha256::Hash::engine();
    engine.input(&transaction);
    engine.input(attempt);
    Ok(sha256::Hash::from_engine(engine).to_byte_array())
}

pub struct Fetch {
    store: LockedCache,
    policy_text: String,
    policy: WalletDescriptor,
    owned: Vec<usize>,
    template: Psbt,
    pending: Session,
    remaining: usize,
    completed: Vec<Session>,
}

impl Fetch {
    pub fn begin(
        fingerprint: Fingerprint,
        policy_text: &str,
        quantity: usize,
    ) -> Result<Self, String> {
        Self::begin_at(
            &datadir().join("ledger-nonces"),
            fingerprint,
            policy_text,
            quantity,
        )
    }

    fn begin_at(
        root: &Path,
        fingerprint: Fingerprint,
        policy_text: &str,
        quantity: usize,
    ) -> Result<Self, String> {
        if !(1..=MAX_LEDGER_NONCE_SESSIONS).contains(&quantity) {
            return Err("nonce quantity must be between 1 and 8".into());
        }
        let policy = descriptor(policy_text)?;
        let owned = owned_expressions(&policy, fingerprint)?;
        let template = synthetic(&policy, fingerprint)?;
        let mut store = LockedCache::open(root, fingerprint)?;
        store.invalidate()?;
        let pending = Self::reserve(&mut store, policy_text)?;
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

    fn reserve(store: &mut LockedCache, policy: &str) -> Result<Session, String> {
        let mut id = [0; 32];
        getrandom::getrandom(&mut id).map_err(|e| e.to_string())?;
        Ok(Session {
            id,
            order: store.reserve_order()?,
            policy: policy.to_owned(),
            nonces: Vec::new(),
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
        self.pending.nonces = nonces;
        self.completed.push(self.pending.clone());
        self.remaining -= 1;
        if self.remaining == 0 {
            let mut next = self.store.state.clone();
            next.available = self.completed.clone();
            self.store.save(next)?;
            return Ok(true);
        }
        self.pending = Self::reserve(&mut self.store, &self.policy_text)?;
        Ok(false)
    }
}

// own_slots validates input ownership. Match its exact slots to public derivations,
// retaining occurrence indexes rather than reconstructing bwk's private input resolver.
fn requested_slots(
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
    let mut result = Vec::new();
    for slot in requested {
        slot_sighash(real, &slot).map_err(|e| e.to_string())?;
        let indexes: BTreeSet<_> = real.inputs[slot.input_index]
            .tap_key_origins
            .values()
            .filter_map(|(_, (_, path))| match path.as_ref().last() {
                Some(ChildNumber::Normal { index }) => Some(*index),
                _ => None,
            })
            .collect();
        let mut matches = BTreeMap::new();
        for single in &singles {
            for index in &indexes {
                let definite = single.derive_at(*index).map_err(|e| e.to_string())?;
                for entry in expression_slots(&definite, slot.input_index, &owned, fingerprint)? {
                    if entry.slot == slot {
                        matches.insert(entry.expression, entry);
                    }
                }
            }
        }
        if matches.len() != 1 {
            return Err("unsupported or ambiguous Ledger MuSig expression coverage".into());
        }
        result.extend(matches.into_values());
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

pub enum Signing {
    Nonces(Vec<u8>),
    Missing,
    Payload(Psbt),
}

pub fn prepare(
    fingerprint: Fingerprint,
    policy_text: &str,
    bytes: &[u8],
) -> Result<Signing, SigningError> {
    prepare_at(
        &datadir().join("ledger-nonces"),
        fingerprint,
        policy_text,
        bytes,
    )
}

fn prepare_at(
    root: &Path,
    fingerprint: Fingerprint,
    policy_text: &str,
    bytes: &[u8],
) -> Result<Signing, SigningError> {
    let mut v2 = psbt::parse_v2(bytes).map_err(|e| e.to_string())?;
    let real = v2.clone().into_bitcoin_psbt().map_err(|e| e.to_string())?;
    let policy = descriptor(policy_text)?;
    let slots = requested_slots(&real, &policy, fingerprint)?;
    let binding = binding(&real, &policy, fingerprint)?;
    let mut store = LockedCache::open(root, fingerprint)?;
    let round1 = musig_status(&real, &policy)
        .map_err(|e| e.to_string())?
        .round
        == MusigRound::Nonces;
    let Some(allocation) = store.allocate(binding, policy_text, &slots, round1)? else {
        return Ok(Signing::Missing);
    };
    let expected: BTreeSet<_> = slots.iter().map(|entry| entry.slot).collect();
    let allocated: BTreeSet<_> = allocation.slots.iter().map(PublishedNonce::slot).collect();
    if expected != allocated {
        return Err(SigningError::SessionLost(
            "MuSig slots changed after nonce allocation".into(),
        ));
    }
    for published in &allocation.slots {
        let field = published.field()?;
        let existing = musig_input_pub_nonce(
            &real.inputs[published.input],
            published.participant,
            published.aggregate,
            published.leaf_hash,
        )
        .map_err(|e| e.to_string())?;
        if existing.is_some_and(|nonce| nonce != field.nonce) || (!round1 && existing.is_none()) {
            return Err(SigningError::SessionLost(
                "transaction nonce does not match its Ledger allocation".into(),
            ));
        }
        set_musig_input_pub_nonce(&mut v2.inputs[published.input].psbt, &field);
    }
    if round1 {
        return Ok(Signing::Nonces(v2.serialize().map_err(|e| e.to_string())?));
    }
    store.bind_round2(&allocation, &real)?;
    let mut private = v2.into_bitcoin_psbt().map_err(|e| e.to_string())?;
    private
        .proprietary
        .insert(session_key(), allocation.id.to_vec());
    Ok(Signing::Payload(private))
}

fn nonce_transcript(psbt: &Psbt) -> Result<[u8; 32], String> {
    let mut engine = sha256::Hash::engine();
    for (index, input) in psbt.inputs.iter().enumerate() {
        engine.input(&(index as u64).to_le_bytes());
        let fields = musig_input_pub_nonces(input).map_err(|e| e.to_string())?;
        engine.input(&(fields.len() as u64).to_le_bytes());
        for field in fields {
            engine.input(&field.participant.serialize());
            engine.input(&field.aggregate.serialize());
            engine.input(&[u8::from(field.leaf_hash.is_some())]);
            if let Some(leaf) = field.leaf_hash {
                engine.input(leaf.as_byte_array());
            }
            engine.input(&field.nonce);
        }
    }
    Ok(sha256::Hash::from_engine(engine).to_byte_array())
}

pub fn signing_status(
    fingerprint: Fingerprint,
    policy_text: &str,
    real: &Psbt,
) -> Result<Option<MusigStatus>, String> {
    let parsed = Descriptor::from_str(policy_text).map_err(|e| e.to_string())?;
    let Some(policy) = parsed.as_miniscript() else {
        return Ok(None);
    };
    if !policy.has_musig()
        || own_slots(real, policy, fingerprint)
            .map_err(|e| e.to_string())?
            .is_empty()
    {
        return Ok(None);
    }
    musig_status(real, policy)
        .map(Some)
        .map_err(|e| e.to_string())
}

pub fn requested_by(real: &Psbt, fingerprint: Fingerprint) -> Result<bool, String> {
    for input in &real.inputs {
        for aggregate in musig_input_participants_all(input).map_err(|e| e.to_string())? {
            for participant in aggregate.participants {
                if input
                    .tap_key_origins
                    .get(&participant.x_only_public_key().0)
                    .is_some_and(|(_, (fp, _))| *fp == fingerprint)
                {
                    return Ok(true);
                }
            }
        }
    }
    Ok(false)
}

pub fn check_round2(
    real: &Psbt,
    policy: &WalletDescriptor,
    fingerprint: Fingerprint,
) -> Result<(), String> {
    for entry in requested_slots(real, policy, fingerprint)? {
        let slot = entry.slot;
        if musig_input_partial_sig(
            &real.inputs[slot.input_index],
            slot.participant,
            slot.aggregate,
            slot.leaf_hash,
        )
        .map_err(|e| e.to_string())?
        .is_none()
        {
            return Err("Ledger returned incomplete MuSig partial signatures".into());
        }
    }
    Ok(())
}

pub fn finish(fingerprint: Fingerprint, policy: &str, bytes: &[u8]) -> Result<(), SigningError> {
    finish_at(
        &datadir().join("ledger-nonces"),
        fingerprint,
        policy,
        bytes,
        AllocationState::Consumed,
    )
}

pub fn invalidate_signing(
    fingerprint: Fingerprint,
    policy: &str,
    bytes: &[u8],
) -> Result<(), SigningError> {
    finish_at(
        &datadir().join("ledger-nonces"),
        fingerprint,
        policy,
        bytes,
        AllocationState::Invalid,
    )
}

fn finish_at(
    root: &Path,
    fingerprint: Fingerprint,
    policy_text: &str,
    bytes: &[u8],
    state: AllocationState,
) -> Result<(), SigningError> {
    let real = psbt::parse_v2(bytes)
        .map_err(|e| e.to_string())?
        .into_bitcoin_psbt()
        .map_err(|e| e.to_string())?;
    let policy = descriptor(policy_text)?;
    let binding = binding(&real, &policy, fingerprint)?;
    let mut store = LockedCache::open(root, fingerprint)?;
    let Some(allocation) = store
        .state
        .allocated
        .iter()
        .find(|entry| entry.binding == binding)
    else {
        return Err(SigningError::SessionLost(
            "Ledger nonce allocation is missing".into(),
        ));
    };
    if !allocation.compatible_nonces(&real, true)? {
        return Err(SigningError::SessionLost(
            "request does not match the active Ledger nonce allocation".into(),
        ));
    }
    if allocation.state == AllocationState::Invalid {
        return Err(SigningError::SessionLost(
            "Ledger signing attempt was invalidated".into(),
        ));
    }
    if state == AllocationState::Consumed {
        let transcript = nonce_transcript(&real)?;
        if allocation.round2_nonces != Some(transcript) {
            if allocation.state == AllocationState::Active && allocation.round2_nonces.is_some() {
                store.retire(binding, AllocationState::Invalid)?;
            }
            return Err(SigningError::SessionLost(
                "signed request does not match its Ledger round-two transcript".into(),
            ));
        }
    }
    store.retire(binding, state)?;
    Ok(())
}

pub fn restart(
    policy_text: &str,
    original: &[u8],
    prepared: &[u8],
    fingerprints: &[Fingerprint],
) -> Result<Vec<u8>, String> {
    restart_at(
        &datadir().join("ledger-nonces"),
        policy_text,
        original,
        prepared,
        fingerprints,
    )
}

fn restart_at(
    root: &Path,
    policy_text: &str,
    original: &[u8],
    prepared: &[u8],
    fingerprints: &[Fingerprint],
) -> Result<Vec<u8>, String> {
    let real = psbt::parse_v2(original)
        .map_err(|e| e.to_string())?
        .into_bitcoin_psbt()
        .map_err(|e| e.to_string())?;
    let mut next = psbt::parse_v2(prepared).map_err(|e| e.to_string())?;
    let target = next
        .clone()
        .into_bitcoin_psbt()
        .map_err(|e| e.to_string())?;
    let policy = descriptor(policy_text)?;
    // A restart may clear nonce fields, but must still name exactly the same transaction.
    if session_id(&real, &policy, Fingerprint::default()).map_err(|e| e.to_string())?
        != session_id(&target, &policy, Fingerprint::default()).map_err(|e| e.to_string())?
    {
        return Err("restart PSBT names a different transaction".into());
    }
    let mut attempt = [0; 32];
    getrandom::getrandom(&mut attempt).map_err(|e| e.to_string())?;
    for fingerprint in fingerprints {
        retire_restart_binding(root, &policy, &real, *fingerprint)?;
    }
    for input in &mut next.inputs {
        bwk::bwk_tx::bwk_psbt::musig::remove_musig_input_pub_nonces(&mut input.psbt);
        bwk::bwk_tx::bwk_psbt::musig::remove_musig_input_partial_sigs(&mut input.psbt);
    }
    next.proprietary.insert(attempt_key(), attempt.to_vec());
    next.serialize().map_err(|e| e.to_string())
}

fn retire_restart_binding(
    root: &Path,
    policy: &WalletDescriptor,
    real: &Psbt,
    fingerprint: Fingerprint,
) -> Result<(), String> {
    if own_slots(real, policy, fingerprint)
        .map_err(|e| e.to_string())?
        .is_empty()
    {
        return Ok(());
    }
    let binding = binding(real, policy, fingerprint)?;
    let mut store = LockedCache::open(root, fingerprint)?;
    if let Some(allocation) = store
        .state
        .allocated
        .iter()
        .find(|entry| entry.binding == binding)
    {
        if !allocation.compatible_nonces(real, false)? {
            // A stale request cannot retire another allocation's published nonce.
            return Ok(());
        }
    }
    store.retire(binding, AllocationState::Invalid)
}

pub fn can_publish(
    fingerprint: Fingerprint,
    policy_text: &str,
    real: &Psbt,
) -> Result<bool, String> {
    let snapshot = SNAPSHOTS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .get(&fingerprint)
        .cloned();
    let Some(snapshot) = snapshot else {
        return Ok(false);
    };
    let policy = descriptor(policy_text)?;
    if musig_status(real, &policy)
        .map_err(|e| e.to_string())?
        .round
        != MusigRound::Nonces
    {
        return Ok(false);
    }
    let binding = binding(real, &policy, fingerprint)?;
    if snapshot.retired.contains(&binding) {
        return Ok(false);
    }
    let allocation = snapshot
        .allocated
        .iter()
        .find(|entry| entry.binding == binding);
    let session = snapshot
        .available
        .iter()
        .filter(|session| session.policy == policy_text)
        .min_by_key(|session| session.order);
    if allocation.is_none() && session.is_none() {
        return Ok(false);
    }
    let slots = requested_slots(real, &policy, fingerprint)?;
    if let Some(allocation) = allocation {
        return Ok(allocation.state == AllocationState::Active
            && allocation
                .slots
                .iter()
                .map(PublishedNonce::slot)
                .collect::<BTreeSet<_>>()
                == slots
                    .iter()
                    .map(|entry| entry.slot)
                    .collect::<BTreeSet<_>>()
            && allocation.compatible_nonces(real, false)?);
    }
    if let Some(session) = session {
        session.published(&slots)?;
        return Ok(true);
    }
    Ok(false)
}

#[cfg(test)]
mod tests {
    use std::{collections::BTreeSet, fs, path::Path, str::FromStr};

    use bitcoin::{
        bip32::{DerivationPath, Fingerprint, Xpriv, Xpub},
        secp256k1::{PublicKey, Secp256k1, SecretKey},
        taproot::{LeafVersion, TapLeafHash},
        Amount, Network, Psbt, TxOut,
    };
    use bwk::{
        bwk_descriptor::{updater::update_input, wallet::WalletDescriptorExt},
        bwk_sign::{
            miniscript,
            musig::{own_slots, MusigRound},
        },
        bwk_tx::bwk_psbt::{
            musig::{
                musig_input_pub_nonces, remove_musig_input_pub_nonces, set_musig_input_partial_sig,
                set_musig_input_pub_nonce, MusigPartialSig, MusigPubNonce,
            },
            PsbtV2,
        },
    };
    use temp_dir::TempDir;

    use crate::{
        keys::{
            ledger_nonces::{
                binding, can_publish, check_round2, count, descriptor, expression_slots, finish_at,
                owned_expressions, prepare_at, refresh_at, restart_at, session_key, signing_status,
                start_attempt, synthetic, AllocationState, Fetch, LockedCache, Signing,
                SigningError, FETCH_FEE_SATS, FUNDING_SATS, MAX_INPUT, SNAPSHOTS,
            },
            manager::{KeyManager, KeyManagerImpl, UnusableReason},
            record::{DeviceRecord, KeyRecord, RouteKind},
            store::KeyStore,
            CAPABILITY_MUSIG,
        },
        state_crypt,
    };

    struct Policy {
        text: String,
        fingerprint: Fingerprint,
    }

    fn key(seed: u8) -> (String, Fingerprint) {
        let secp = Secp256k1::new();
        let master = Xpriv::new_master(Network::Regtest, &[seed; 64]).expect("master key");
        let fingerprint = master.fingerprint(&secp);
        let path = DerivationPath::from_str("m/87'/1'/0'/2").expect("origin path");
        let child = master.derive_priv(&secp, &path).expect("account key");
        let xpub = Xpub::from_priv(&secp, &child);
        (format!("[{fingerprint}/87'/1'/0'/2]{xpub}"), fingerprint)
    }

    fn policy(tree: bool, zone: u32) -> Policy {
        let (a, fingerprint) = key(1);
        let (b, _) = key(2);
        let (c, _) = key(3);
        let first = zone * 2;
        let second = first + 1;
        let text = if tree {
            format!("tr(musig({a},{b})/<{first};{second}>/*,{{pk(musig({a},{b})/<512;513>/*),pk(musig({a},{c})/<1024;1025>/*)}})")
        } else {
            format!("tr(musig({a},{b})/<{first};{second}>/*)")
        };
        Policy { text, fingerprint }
    }

    fn simple_policy(first_seed: u8, second_seed: u8, zone: u32) -> Policy {
        let (a, fingerprint) = key(first_seed);
        let (b, _) = key(second_seed);
        let first = zone * 2;
        let second = first + 1;
        Policy {
            text: format!("tr(musig({a},{b})/<{first};{second}>/*)"),
            fingerprint,
        }
    }

    fn as_v0(bytes: &[u8]) -> Psbt {
        PsbtV2::deserialize(bytes)
            .expect("v2 PSBT")
            .into_bitcoin_psbt()
            .expect("v0 PSBT")
    }

    fn complete_nonces(policy: &Policy, real: &mut Psbt) {
        let parsed = descriptor(&policy.text).expect("policy");
        let fingerprints: BTreeSet<_> = parsed
            .plain_keys()
            .iter()
            .map(|key| key.master_fingerprint())
            .collect();
        for fingerprint in fingerprints {
            if fingerprint == policy.fingerprint {
                continue;
            }
            for slot in own_slots(real, &parsed, fingerprint).expect("peer slots") {
                set_musig_input_pub_nonce(
                    &mut real.inputs[slot.input_index],
                    &MusigPubNonce {
                        participant: slot.participant,
                        aggregate: slot.aggregate,
                        leaf_hash: slot.leaf_hash,
                        nonce: nonce(30, slot.input_index, 0),
                    },
                );
            }
        }
    }

    fn ledger_manager(root: &Path, policies: &[&Policy]) -> KeyManager {
        let mut manager = KeyManager::new(
            KeyStore::new(root.join("keys"), Network::Regtest),
            Network::Regtest,
        )
        .expect("manager");
        let fingerprint = policies[0].fingerprint;
        let mut record = KeyRecord::new(fingerprint, Network::Regtest, "Ledger".into());
        record.routes.push(RouteKind::Usb).expect("USB route");
        record.devices.push(DeviceRecord {
            kind: "ledger".into(),
            model: String::new(),
            serial: String::new(),
            module: "hwi".into(),
            version: String::new(),
            capabilities: CAPABILITY_MUSIG,
        });
        manager.add_key(record).expect("Ledger key");
        for policy in policies {
            manager
                .set_descriptor_registered(fingerprint, "account".into(), policy.text.clone())
                .expect("registered policy");
        }
        manager
    }

    fn nonce(tag: u8, input: usize, expression: usize) -> [u8; 66] {
        let secret = SecretKey::from_slice(&[tag + input as u8 + expression as u8 + 1; 32])
            .expect("nonce secret");
        let point = PublicKey::from_secret_key(&Secp256k1::new(), &secret).serialize();
        let mut bytes = [0; 66];
        bytes[..33].copy_from_slice(&point);
        bytes[33..].copy_from_slice(&point);
        bytes
    }

    fn answer(fetch: &Fetch, tag: u8) -> Psbt {
        let mut signed = fetch.payload();
        let definite = fetch.policy.single_paths().expect("paths")[0]
            .derive_at(0)
            .expect("derive");
        for input in 0..MAX_INPUT {
            for entry in expression_slots(
                &definite,
                input,
                &fetch.owned,
                fetch.store.state.fingerprint,
            )
            .expect("expression slots")
            {
                set_musig_input_pub_nonce(
                    &mut signed.inputs[input],
                    &MusigPubNonce {
                        participant: entry.slot.participant,
                        aggregate: entry.slot.aggregate,
                        leaf_hash: entry.slot.leaf_hash,
                        nonce: nonce(tag, input, entry.expression),
                    },
                );
            }
        }
        signed
    }

    fn real(policy: &Policy, branch: usize, index: u32, inputs: usize) -> Psbt {
        let parsed = descriptor(&policy.text).expect("policy");
        let definite = parsed.single_paths().expect("paths")[branch]
            .derive_at(index)
            .expect("derive");
        let mut psbt = synthetic(&parsed, policy.fingerprint).expect("synthetic transaction");
        psbt.unsigned_tx.input.truncate(inputs);
        psbt.inputs.truncate(inputs);
        psbt.unsigned_tx.output[0].value =
            Amount::from_sat(FUNDING_SATS * inputs as u64 - FETCH_FEE_SATS);
        for input in 0..inputs {
            psbt.inputs[input] = bitcoin::psbt::Input::default();
            psbt.inputs[input].witness_utxo = Some(TxOut {
                value: Amount::from_sat(FUNDING_SATS),
                script_pubkey: definite.script_pubkey(),
            });
            update_input(&mut psbt, input, &definite).expect("public descriptor updater");
        }
        psbt
    }

    fn v2(psbt: Psbt) -> Vec<u8> {
        PsbtV2::from_bitcoin_psbt(psbt)
            .expect("v2 conversion")
            .serialize()
            .expect("v2 bytes")
    }

    fn publish(root: &Path, policy: &Policy, psbt: Psbt) -> Vec<u8> {
        match prepare_at(root, policy.fingerprint, &policy.text, &v2(psbt)).expect("allocate") {
            Signing::Nonces(bytes) => bytes,
            _ => panic!("expected cached nonce publication"),
        }
    }

    #[test]
    fn consumed_attempt_requires_restart_and_uses_a_fresh_session() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = policy(false, 0);
        let mut fetch =
            Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 2).expect("two sessions");
        let first = fetch.pending.id;
        assert!(!fetch.accept(&answer(&fetch, 1)).expect("first command"));
        let second = fetch.pending.id;
        assert!(fetch.accept(&answer(&fetch, 2)).expect("second command"));
        drop(fetch);
        let mut transaction = real(&policy, 1, 17, 1);
        start_attempt(&mut transaction).expect("initial account signing attempt");
        let prepared = v2(transaction.clone());
        let published = publish(tmp.path(), &policy, transaction);
        let mut complete = as_v0(&published);
        complete_nonces(&policy, &mut complete);
        let complete = v2(complete);
        assert!(matches!(
            prepare_at(tmp.path(), policy.fingerprint, &policy.text, &complete).expect("round two"),
            Signing::Payload(_)
        ));
        finish_at(
            tmp.path(),
            policy.fingerprint,
            &policy.text,
            &complete,
            AllocationState::Consumed,
        )
        .expect("durably consume success");
        let reopened =
            LockedCache::open(tmp.path(), policy.fingerprint).expect("restart application");
        assert_eq!(
            reopened.state.allocated[0].state,
            AllocationState::Consumed,
            "successful round two is terminal on disk"
        );
        assert_eq!(
            reopened.state.available.len(),
            1,
            "only unused sessions remain available"
        );
        drop(reopened);
        assert!(
            matches!(
                prepare_at(tmp.path(), policy.fingerprint, &policy.text, &prepared),
                Err(SigningError::SessionLost(_))
            ),
            "nonce-free retry must not resurrect the consumed session"
        );
        let restarted = restart_at(
            tmp.path(),
            &policy.text,
            &complete,
            &prepared,
            &[policy.fingerprint],
        )
        .expect("explicit restart");
        let parsed = descriptor(&policy.text).expect("policy");
        assert_ne!(
            binding(&as_v0(&prepared), &parsed, policy.fingerprint).expect("old binding"),
            binding(&as_v0(&restarted), &parsed, policy.fingerprint).expect("fresh binding"),
            "restart changes the attempt, not the transaction"
        );
        let next = publish(tmp.path(), &policy, as_v0(&restarted));
        assert_ne!(next, published, "new attempt publishes fresh nonce bytes");
        let reopened =
            LockedCache::open(tmp.path(), policy.fingerprint).expect("reopen new attempt");
        assert_eq!(
            reopened.state.allocated[0].id, first,
            "old allocation remains terminal"
        );
        assert_eq!(
            reopened.state.allocated[1].id, second,
            "FIFO unused session serves the new attempt"
        );
        assert!(
            reopened.state.available.is_empty(),
            "consumed sessions never return to availability"
        );
    }

    #[test]
    fn session_loss_retires_only_the_matching_transaction_and_own_nonce() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = policy(false, 0);
        let mut fetch =
            Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 2).expect("sessions");
        assert!(!fetch.accept(&answer(&fetch, 1)).expect("first command"));
        assert!(fetch.accept(&answer(&fetch, 2)).expect("second command"));
        drop(fetch);
        let first = real(&policy, 0, 5, 1);
        let mut second = first.clone();
        second.unsigned_tx.output[0].value =
            Amount::from_sat(second.unsigned_tx.output[0].value.to_sat() - 1);
        let first = publish(tmp.path(), &policy, first);
        let second = publish(tmp.path(), &policy, second);
        let mut wrong = as_v0(&first);
        let mut field = musig_input_pub_nonces(&wrong.inputs[0]).expect("own nonce")[0];
        field.nonce = nonce(50, 0, 0);
        set_musig_input_pub_nonce(&mut wrong.inputs[0], &field);
        let wrong = v2(wrong);
        assert!(
            matches!(
                finish_at(
                    tmp.path(),
                    policy.fingerprint,
                    &policy.text,
                    &wrong,
                    AllocationState::Invalid
                ),
                Err(SigningError::SessionLost(_))
            ),
            "mismatched own nonce is a stale request"
        );
        restart_at(
            tmp.path(),
            &policy.text,
            &wrong,
            &first,
            &[policy.fingerprint],
        )
        .expect("restart stale request without retiring another nonce");
        let reopened =
            LockedCache::open(tmp.path(), policy.fingerprint).expect("unchanged allocations");
        assert!(
            reopened
                .state
                .allocated
                .iter()
                .all(|allocation| allocation.state == AllocationState::Active),
            "stale nonce cannot invalidate either active transaction"
        );
        drop(reopened);
        finish_at(
            tmp.path(),
            policy.fingerprint,
            &policy.text,
            &first,
            AllocationState::Invalid,
        )
        .expect("retire lost device session");
        let reopened = LockedCache::open(tmp.path(), policy.fingerprint).expect("durable loss");
        assert_eq!(
            reopened.state.allocated[0].state,
            AllocationState::Invalid,
            "matching device session is retired"
        );
        assert_eq!(
            reopened.state.allocated[1].state,
            AllocationState::Active,
            "other transaction remains active"
        );
        drop(reopened);
        assert_eq!(
            publish(tmp.path(), &policy, as_v0(&second)),
            second,
            "other transaction keeps its idempotent round-one answer"
        );
    }

    #[test]
    fn explicit_restart_retires_all_ledger_fingerprints_before_new_round_one() {
        let tmp = TempDir::new().expect("temporary cache");
        let a = policy(false, 0);
        let (_, b_fingerprint) = key(2);
        let b = Policy {
            text: a.text.clone(),
            fingerprint: b_fingerprint,
        };
        for policy in [&a, &b] {
            let mut fetch = Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 2)
                .expect("device sessions");
            assert!(!fetch.accept(&answer(&fetch, 1)).expect("first command"));
            assert!(fetch.accept(&answer(&fetch, 2)).expect("second command"));
        }
        let prepared = v2(real(&a, 1, 17, 1));
        let first = publish(tmp.path(), &a, as_v0(&prepared));
        let original = publish(tmp.path(), &b, as_v0(&first));
        let restarted = restart_at(
            tmp.path(),
            &a.text,
            &original,
            &prepared,
            &[a.fingerprint, b.fingerprint],
        )
        .expect("retire both devices");
        assert!(
            musig_input_pub_nonces(&as_v0(&restarted).inputs[0])
                .expect("nonce-free restart")
                .is_empty(),
            "restart drops every published nonce"
        );
        for policy in [&a, &b] {
            let reopened =
                LockedCache::open(tmp.path(), policy.fingerprint).expect("retirement on disk");
            assert_eq!(
                reopened.state.allocated[0].state,
                AllocationState::Invalid,
                "every device's old binding is invalid"
            );
            assert_eq!(
                reopened.state.available.len(),
                1,
                "unused device session remains available"
            );
            drop(reopened);
            assert!(
                matches!(
                    prepare_at(tmp.path(), policy.fingerprint, &policy.text, &prepared),
                    Err(SigningError::SessionLost(_))
                ),
                "late old round-one request cannot revive a retired attempt"
            );
        }
        let next = publish(tmp.path(), &a, as_v0(&restarted));
        let next = publish(tmp.path(), &b, as_v0(&next));
        assert_eq!(
            musig_input_pub_nonces(&as_v0(&next).inputs[0])
                .expect("fresh round")
                .len(),
            2,
            "both devices can publish a fresh round after retirement"
        );
    }

    #[test]
    fn restart_fences_an_old_request_that_has_not_allocated_yet() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = policy(false, 0);
        let mut fetch =
            Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 1).expect("session");
        assert!(fetch.accept(&answer(&fetch, 1)).expect("command"));
        drop(fetch);
        let prepared = v2(real(&policy, 0, 7, 1));
        let restarted = restart_at(
            tmp.path(),
            &policy.text,
            &prepared,
            &prepared,
            &[policy.fingerprint],
        )
        .expect("restart before old worker allocation");
        assert!(
            matches!(
                prepare_at(tmp.path(), policy.fingerprint, &policy.text, &prepared),
                Err(SigningError::SessionLost(_))
            ),
            "late old worker is fenced durably even without an old allocation"
        );
        let reopened =
            LockedCache::open(tmp.path(), policy.fingerprint).expect("tombstone persists");
        assert_eq!(
            reopened.state.retired.len(),
            1,
            "retired unpublished attempt is remembered"
        );
        assert_eq!(
            reopened.state.available.len(),
            1,
            "late request consumes no session"
        );
        drop(reopened);
        publish(tmp.path(), &policy, as_v0(&restarted));
    }

    #[test]
    fn failed_restart_returns_no_new_attempt_and_retry_finishes_all_retirements() {
        let tmp = TempDir::new().expect("temporary cache");
        let a = policy(false, 0);
        let (_, b_fingerprint) = key(2);
        let b = Policy {
            text: a.text.clone(),
            fingerprint: b_fingerprint,
        };
        for policy in [&a, &b] {
            let mut fetch =
                Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 1).expect("session");
            assert!(fetch.accept(&answer(&fetch, 1)).expect("command"));
        }
        let prepared = v2(real(&a, 0, 5, 1));
        let first = publish(tmp.path(), &a, as_v0(&prepared));
        let original = publish(tmp.path(), &b, as_v0(&first));
        let blocked = LockedCache::open(tmp.path(), b.fingerprint)
            .expect("second device store")
            .path
            .with_extension("tmp");
        fs::create_dir(&blocked).expect("force retirement write failure");
        assert!(
            restart_at(
                tmp.path(),
                &a.text,
                &original,
                &prepared,
                &[a.fingerprint, b.fingerprint]
            )
            .is_err(),
            "no new attempt PSBT is returned before every device retirement reaches disk"
        );
        let first = LockedCache::open(tmp.path(), a.fingerprint).expect("first device");
        assert_eq!(
            first.state.allocated[0].state,
            AllocationState::Invalid,
            "successful retirement remains durable"
        );
        drop(first);
        let second = LockedCache::open(tmp.path(), b.fingerprint).expect("second device");
        assert_eq!(
            second.state.allocated[0].state,
            AllocationState::Active,
            "failed write leaves the old allocation intact"
        );
        drop(second);
        fs::remove_dir(&blocked).expect("restore storage");
        let restarted = restart_at(
            tmp.path(),
            &a.text,
            &original,
            &prepared,
            &[a.fingerprint, b.fingerprint],
        )
        .expect("retry completes retirement");
        assert_ne!(
            restarted, prepared,
            "successful retry returns a fresh attempt ID"
        );
        let second =
            LockedCache::open(tmp.path(), b.fingerprint).expect("second retirement persisted");
        assert_eq!(
            second.state.allocated[0].state,
            AllocationState::Invalid,
            "retry retires the remaining device"
        );
    }

    #[test]
    fn offline_plan_resumes_a_persisted_active_allocation_with_zero_available_sessions() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = simple_policy(60, 61, 0);
        let manager = ledger_manager(tmp.path(), &[&policy]);
        let mut fetch =
            Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 1).expect("session");
        assert!(fetch.accept(&answer(&fetch, 1)).expect("command"));
        drop(fetch);
        let transaction = real(&policy, 1, 17, 1);
        let prepared = v2(transaction.clone());
        let published = publish(tmp.path(), &policy, transaction);
        SNAPSHOTS
            .lock()
            .expect("snapshots")
            .remove(&policy.fingerprint);
        refresh_at(tmp.path(), policy.fingerprint)
            .expect("application restart loads durable allocation");
        assert_eq!(
            count(policy.fingerprint, &policy.text),
            0,
            "display count remains available-only after allocating the last session"
        );
        let plan = manager
            .signing_plan(&prepared)
            .expect("transaction-aware offline plan");
        let candidate = plan
            .candidates
            .iter()
            .find(|candidate| candidate.fingerprint == policy.fingerprint)
            .expect("Ledger candidate");
        assert!(candidate.unusable.is_none(), "active transaction allocation enables offline round-one retry with no available sessions");
        assert_eq!(
            candidate.route,
            Some(RouteKind::Usb),
            "cache-backed USB route"
        );
        assert!(
            can_publish(policy.fingerprint, &policy.text, &as_v0(&prepared))
                .expect("cached capability"),
            "active binding covers this transaction"
        );
        assert_eq!(
            publish(tmp.path(), &policy, as_v0(&prepared)),
            published,
            "offline retry returns the same public nonces"
        );
        let mut other = as_v0(&prepared);
        other.unsigned_tx.output[0].value =
            Amount::from_sat(other.unsigned_tx.output[0].value.to_sat() - 1);
        let plan = manager
            .signing_plan(&v2(other))
            .expect("other transaction plan");
        assert_eq!(
            plan.candidates
                .iter()
                .find(|candidate| candidate.fingerprint == policy.fingerprint)
                .expect("Ledger")
                .unusable,
            Some(UnusableReason::DeviceNotConnected),
            "active allocation cannot enable an unrelated transaction"
        );
        let mut partial = as_v0(&published);
        complete_nonces(&policy, &mut partial);
        let plan = manager.signing_plan(&v2(partial)).expect("round-two plan");
        assert_eq!(
            plan.candidates
                .iter()
                .find(|candidate| candidate.fingerprint == policy.fingerprint)
                .expect("Ledger")
                .unusable,
            Some(UnusableReason::DeviceNotConnected),
            "active cache allocation never enables offline round two"
        );
    }

    #[test]
    fn ledger_cache_coverage_does_not_disable_an_existing_qr_route() {
        let tmp = TempDir::new().expect("temporary cache");
        let (a, fingerprint) = key(68);
        let (b, _) = key(69);
        let policy = Policy {
            text: format!("tr(musig({a}/<0;1>/*,{b}/<0;1>/*))"),
            fingerprint,
        };
        let mut manager = ledger_manager(tmp.path(), &[&policy]);
        manager
            .add_route(fingerprint, RouteKind::Qr)
            .expect("usable QR route");
        let mut fetch =
            Fetch::begin_at(tmp.path(), fingerprint, &policy.text, 1).expect("Ledger session");
        assert!(fetch.accept(&answer(&fetch, 1)).expect("Ledger command"));
        drop(fetch);
        let plan = manager
            .signing_plan(&v2(real(&policy, 1, 17, 1)))
            .expect("non-cached route still works");
        let candidate = plan
            .candidates
            .iter()
            .find(|candidate| candidate.fingerprint == fingerprint)
            .expect("registered key");
        assert_eq!(
            candidate.route,
            Some(RouteKind::Qr),
            "keep the existing usable route"
        );
        assert!(
            candidate.unusable.is_none(),
            "Ledger's cache limits must not reject another route's policy"
        );
        assert!(
            !candidate.musig_done,
            "the requested nonce contribution is still missing"
        );
    }

    #[test]
    fn completion_and_offline_eligibility_cover_every_requested_policy() {
        let tmp = TempDir::new().expect("temporary cache");
        let a = simple_policy(64, 65, 0);
        let b = simple_policy(64, 65, 1);
        let manager = ledger_manager(tmp.path(), &[&a, &b]);
        let mut transaction = real(&a, 0, 5, 1);
        let second = real(&b, 1, 17, 1);
        transaction
            .unsigned_tx
            .input
            .push(second.unsigned_tx.input[0].clone());
        transaction.inputs.push(second.inputs[0].clone());
        let mut fetch =
            Fetch::begin_at(tmp.path(), a.fingerprint, &a.text, 1).expect("first policy session");
        assert!(fetch
            .accept(&answer(&fetch, 1))
            .expect("first policy command"));
        drop(fetch);
        let first = publish(tmp.path(), &a, transaction);
        let mut fetch =
            Fetch::begin_at(tmp.path(), b.fingerprint, &b.text, 1).expect("second policy session");
        assert!(fetch
            .accept(&answer(&fetch, 2))
            .expect("second policy command"));
        drop(fetch);
        let plan = manager
            .signing_plan(&first)
            .expect("mixed-policy nonce plan");
        let own = plan
            .candidates
            .iter()
            .find(|candidate| candidate.fingerprint == a.fingerprint)
            .expect("Ledger");
        assert!(
            !own.musig_done,
            "first policy's nonce does not complete the second policy"
        );
        assert!(
            own.unusable.is_none(),
            "second policy's cached session can complete round one offline"
        );
        let both = publish(tmp.path(), &b, as_v0(&first));
        let mut complete = as_v0(&both);
        complete_nonces(&a, &mut complete);
        complete_nonces(&b, &mut complete);
        let parsed_a = descriptor(&a.text).expect("first policy");
        let slot = own_slots(&complete, &parsed_a, a.fingerprint).expect("first slot")[0];
        set_musig_input_partial_sig(
            &mut complete.inputs[slot.input_index],
            &MusigPartialSig {
                participant: slot.participant,
                aggregate: slot.aggregate,
                leaf_hash: slot.leaf_hash,
                signature: [7; 32],
            },
        );
        let plan = manager
            .signing_plan(&v2(complete.clone()))
            .expect("mixed-policy partial plan");
        let own = plan
            .candidates
            .iter()
            .find(|candidate| candidate.fingerprint == a.fingerprint)
            .expect("Ledger");
        assert!(
            !own.musig_done,
            "first policy's partial signature cannot complete the entire transaction"
        );
        assert_eq!(
            own.unusable,
            Some(UnusableReason::DeviceNotConnected),
            "round-two completion still needs the device"
        );
        let parsed_b = descriptor(&b.text).expect("second policy");
        let slot = own_slots(&complete, &parsed_b, b.fingerprint).expect("second slot")[0];
        set_musig_input_partial_sig(
            &mut complete.inputs[slot.input_index],
            &MusigPartialSig {
                participant: slot.participant,
                aggregate: slot.aggregate,
                leaf_hash: slot.leaf_hash,
                signature: [7; 32],
            },
        );
        let plan = manager
            .signing_plan(&v2(complete))
            .expect("all own partial signatures");
        assert!(
            plan.candidates
                .iter()
                .find(|candidate| candidate.fingerprint == a.fingerprint)
                .expect("Ledger")
                .musig_done,
            "every requested policy must contribute before the signer is done"
        );
    }

    #[test]
    fn session_loss_is_typed_separately_from_bad_input_and_storage_failure() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = policy(false, 0);
        assert!(
            matches!(
                prepare_at(
                    tmp.path(),
                    policy.fingerprint,
                    &policy.text,
                    b"invalid PSBT"
                ),
                Err(SigningError::Invalid(_))
            ),
            "bad input is not session loss"
        );
        let parsed = descriptor(&policy.text).expect("policy");
        let mut complete = real(&policy, 0, 5, 1);
        for seed in [1, 2] {
            let (_, fingerprint) = key(seed);
            for slot in own_slots(&complete, &parsed, fingerprint).expect("slots") {
                set_musig_input_pub_nonce(
                    &mut complete.inputs[slot.input_index],
                    &MusigPubNonce {
                        participant: slot.participant,
                        aggregate: slot.aggregate,
                        leaf_hash: slot.leaf_hash,
                        nonce: nonce(seed, 0, 0),
                    },
                );
            }
        }
        assert!(
            matches!(
                prepare_at(tmp.path(), policy.fingerprint, &policy.text, &v2(complete)),
                Err(SigningError::SessionLost(_))
            ),
            "round two without allocation requests restart"
        );
        let mut fetch =
            Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 1).expect("session");
        assert!(fetch.accept(&answer(&fetch, 1)).expect("command"));
        let blocked = fetch.store.path.with_extension("tmp");
        drop(fetch);
        fs::create_dir(&blocked).expect("block allocation write");
        assert!(
            matches!(
                prepare_at(
                    tmp.path(),
                    policy.fingerprint,
                    &policy.text,
                    &v2(real(&policy, 0, 5, 1))
                ),
                Err(SigningError::Invalid(_))
            ),
            "storage failure stays an ordinary error instead of restarting rounds"
        );
    }

    #[test]
    fn allocation_is_durable_fifo_and_retries_never_allocate_again() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = policy(false, 0);
        let mut fetch =
            Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 2).expect("fetch");
        let first = fetch.pending.id;
        assert!(
            !fetch
                .accept(&answer(&fetch, 1))
                .expect("first successful command"),
            "batch needs two commands"
        );
        let second = fetch.pending.id;
        assert_ne!(first, second, "each caller session must have its own ID");
        assert!(
            fetch
                .accept(&answer(&fetch, 2))
                .expect("second successful command"),
            "batch complete"
        );
        drop(fetch);
        let mut reordered =
            LockedCache::open(tmp.path(), policy.fingerprint).expect("reorder persisted queue");
        let mut next = reordered.state.clone();
        next.available.reverse();
        reordered
            .save(next)
            .expect("persist reversed storage order");
        drop(reordered);
        let transaction = real(&policy, 1, 17, 2);
        let published = publish(tmp.path(), &policy, transaction.clone());
        let reopened = LockedCache::open(tmp.path(), policy.fingerprint).expect("restart");
        assert_eq!(
            reopened.state.available.len(),
            1,
            "one FIFO session remains"
        );
        assert_eq!(
            reopened.state.allocated.len(),
            1,
            "allocation survives restart before publication"
        );
        assert_eq!(
            reopened.state.allocated[0].id, first,
            "oldest session allocated first"
        );
        assert_eq!(
            reopened.state.available[0].id, second,
            "second remains available"
        );
        assert!(
            state_crypt::is_sealed(&fs::read(&reopened.path).expect("sealed file")),
            "cache is encrypted"
        );
        drop(reopened);
        assert_eq!(
            publish(tmp.path(), &policy, transaction.clone()),
            published,
            "retry returns the same allocated nonces"
        );
        let mut changed = transaction;
        changed.unsigned_tx.output[0].value = Amount::from_sat(198_000);
        publish(tmp.path(), &policy, changed);
        let reopened = LockedCache::open(tmp.path(), policy.fingerprint).expect("restart again");
        assert_eq!(
            reopened.state.allocated.len(),
            2,
            "different transaction gets another session"
        );
        assert!(
            reopened.state.available.is_empty(),
            "allocated sessions never return to availability"
        );
    }

    #[test]
    fn a_fetch_for_another_account_invalidates_available_but_keeps_allocations() {
        let tmp = TempDir::new().expect("temporary cache");
        let a = policy(false, 0);
        let b = policy(false, 1);
        let mut fetch = Fetch::begin_at(tmp.path(), a.fingerprint, &a.text, 2).expect("fetch a");
        assert!(!fetch.accept(&answer(&fetch, 1)).expect("first command"));
        assert!(fetch.accept(&answer(&fetch, 2)).expect("second command"));
        drop(fetch);
        publish(tmp.path(), &a, real(&a, 0, 5, 1));
        let mut replacement =
            Fetch::begin_at(tmp.path(), b.fingerprint, &b.text, 1).expect("fetch b");
        assert!(
            replacement.store.state.available.is_empty(),
            "all account availability cleared before device I/O"
        );
        assert_eq!(
            replacement.store.state.allocated.len(),
            1,
            "pending transaction binding remains"
        );
        assert_eq!(
            replacement.pending.order, 3,
            "fetch counter survives reopen and account change"
        );
        assert!(replacement
            .accept(&answer(&replacement, 3))
            .expect("replacement completed"));
        drop(replacement);
        let reopened =
            LockedCache::open(tmp.path(), a.fingerprint).expect("shared fingerprint store");
        assert_eq!(
            reopened.state.available.len(),
            1,
            "replacement leaves exactly one session"
        );
        assert_eq!(
            reopened.state.available[0].policy, b.text,
            "one account owns availability device-wide"
        );
        assert_eq!(
            reopened.state.allocated.len(),
            1,
            "allocated metadata retained"
        );
    }

    #[test]
    fn incomplete_failed_or_cancelled_fetch_never_commits_a_partial_batch() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = policy(true, 0);
        let mut previous = Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 2)
            .expect("previous batch");
        assert!(!previous
            .accept(&answer(&previous, 7))
            .expect("previous first command"));
        assert!(previous
            .accept(&answer(&previous, 8))
            .expect("previous second command"));
        drop(previous);
        publish(tmp.path(), &policy, real(&policy, 1, 5, 1));
        let mut fetch =
            Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 2).expect("fetch");
        assert!(!fetch.accept(&answer(&fetch, 1)).expect("first command"));
        let mut incomplete = answer(&fetch, 2);
        incomplete.inputs[7].unknown.clear();
        assert!(
            fetch.accept(&incomplete).is_err(),
            "missing coverage must fail the command"
        );
        drop(fetch);
        let reopened = LockedCache::open(tmp.path(), policy.fingerprint).expect("restart");
        assert!(
            reopened.state.available.is_empty(),
            "successful prefix is not an available batch"
        );
        assert_eq!(
            reopened.state.fetch_order, 4,
            "failed command order cannot be reused"
        );
        assert_eq!(
            reopened.state.allocated.len(),
            1,
            "failed replacement preserves the pending transaction binding"
        );
        drop(reopened);
        let fetch = Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 1)
            .expect("cancelled fetch");
        assert!(
            LockedCache::open(tmp.path(), policy.fingerprint).is_err(),
            "device operation holds its fingerprint lock"
        );
        drop(fetch);
        let reopened = LockedCache::open(tmp.path(), policy.fingerprint)
            .expect("lock released after cancellation");
        assert!(
            reopened.state.available.is_empty(),
            "cancelled command creates no availability"
        );
        assert_eq!(reopened.state.fetch_order, 5, "cancelled order is durable");
    }

    #[test]
    fn allocation_write_failure_does_not_publish_nonces() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = policy(false, 0);
        let mut fetch =
            Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 1).expect("fetch");
        assert!(fetch.accept(&answer(&fetch, 1)).expect("fetch completed"));
        let temp = fetch.store.path.with_extension("tmp");
        drop(fetch);
        fs::create_dir(&temp).expect("force atomic write failure");
        assert!(
            prepare_at(
                tmp.path(),
                policy.fingerprint,
                &policy.text,
                &v2(real(&policy, 0, 7, 1))
            )
            .is_err(),
            "failed persistence must return an error rather than public nonces"
        );
        let reopened = LockedCache::open(tmp.path(), policy.fingerprint).expect("restart");
        assert_eq!(
            reopened.state.available.len(),
            1,
            "failed allocation leaves durable queue unchanged"
        );
        assert!(
            reopened.state.allocated.is_empty(),
            "no allocation reached disk"
        );
    }

    #[test]
    fn remaps_input_expression_participant_aggregate_and_leaf_on_disjoint_branches() {
        for branch in [0, 1] {
            let tmp = TempDir::new().expect("temporary cache");
            let policy = policy(true, 0);
            let mut fetch =
                Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 1).expect("fetch");
            let synthetic_fields =
                musig_input_pub_nonces(&answer(&fetch, 9).inputs[0]).expect("synthetic fields");
            assert!(fetch.accept(&answer(&fetch, 9)).expect("fetch completed"));
            drop(fetch);
            let transaction = real(&policy, branch, 17, MAX_INPUT);
            let parsed = descriptor(&policy.text).expect("policy");
            let definite = parsed.single_paths().expect("paths")[branch]
                .derive_at(17)
                .expect("derive");
            let miniscript::Descriptor::Tr(tr) = &definite else {
                panic!("taproot fixture")
            };
            let leaves: Vec<_> = tr
                .iter_scripts()
                .map(|(_, ms)| TapLeafHash::from_script(&ms.encode(), LeafVersion::TapScript))
                .collect();
            let requested =
                own_slots(&transaction, &parsed, policy.fingerprint).expect("upstream real slots");
            assert_eq!(
                requested.len(),
                24,
                "three expressions at all eight input positions"
            );
            let published = publish(tmp.path(), &policy, transaction.clone());
            let signed = PsbtV2::deserialize(&published)
                .expect("published v2")
                .into_bitcoin_psbt()
                .expect("v0");
            assert!(
                !signed.proprietary.contains_key(&session_key()),
                "caller ID stays private"
            );
            for input in 0..MAX_INPUT {
                let fields = musig_input_pub_nonces(&signed.inputs[input]).expect("real fields");
                assert_eq!(
                    fields.len(),
                    3,
                    "internal expression and both distinct repeated-participant leaves"
                );
                for slot in requested.iter().filter(|slot| slot.input_index == input) {
                    let field = fields
                        .iter()
                        .find(|field| {
                            field.aggregate == slot.aggregate
                                && field.leaf_hash == slot.leaf_hash
                                && field.participant == slot.participant
                        })
                        .expect("real slot has its nonce");
                    let expression = match slot.leaf_hash {
                        None => 0,
                        Some(leaf) if leaf == leaves[0] => 1,
                        Some(leaf) if leaf == leaves[1] => 2,
                        Some(_) => panic!("unexpected fixture leaf"),
                    };
                    assert_eq!(
                        field.nonce,
                        nonce(9, input, expression),
                        "nonce follows input and expression, not map iteration order"
                    );
                    assert!(
                        !synthetic_fields
                            .iter()
                            .any(|synthetic| synthetic.aggregate == field.aggregate
                                && synthetic.leaf_hash == field.leaf_hash),
                        "synthetic aggregate/leaf must not be copied unchanged"
                    );
                }
            }
        }
    }

    #[test]
    fn round_two_restores_private_id_and_refuses_changed_nonce_transcript() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = policy(false, 0);
        let mut fetch =
            Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 1).expect("fetch");
        let id = fetch.pending.id;
        assert!(fetch.accept(&answer(&fetch, 1)).expect("fetch completed"));
        drop(fetch);
        let published = publish(tmp.path(), &policy, real(&policy, 1, 17, 1));
        let mut complete = PsbtV2::deserialize(&published)
            .expect("v2")
            .into_bitcoin_psbt()
            .expect("v0");
        let parsed = descriptor(&policy.text).expect("policy");
        let (_, foreign) = key(2);
        let slot = own_slots(&complete, &parsed, foreign).expect("foreign slot")[0];
        let mut field = MusigPubNonce {
            participant: slot.participant,
            aggregate: slot.aggregate,
            leaf_hash: slot.leaf_hash,
            nonce: nonce(5, 0, 0),
        };
        set_musig_input_pub_nonce(&mut complete.inputs[0], &field);
        match prepare_at(
            tmp.path(),
            policy.fingerprint,
            &policy.text,
            &v2(complete.clone()),
        )
        .expect("round two")
        {
            Signing::Payload(private) => assert_eq!(
                private.proprietary[&session_key()],
                id.to_vec(),
                "restore signer-local ID only on private payload"
            ),
            _ => panic!("expected device round-two payload"),
        }
        let reopened = LockedCache::open(tmp.path(), policy.fingerprint).expect("restart");
        assert!(
            reopened.state.allocated[0].round2_nonces.is_some(),
            "transcript bound before device payload leaves"
        );
        drop(reopened);
        field.nonce = nonce(6, 0, 0);
        set_musig_input_pub_nonce(&mut complete.inputs[0], &field);
        let changed = v2(complete);
        assert!(
            matches!(
                prepare_at(tmp.path(), policy.fingerprint, &policy.text, &changed),
                Err(SigningError::SessionLost(_))
            ),
            "changed peer nonces request a session restart, not a permanent ordinary error"
        );
        let reopened =
            LockedCache::open(tmp.path(), policy.fingerprint).expect("retired transcript on disk");
        assert_eq!(
            reopened.state.allocated[0].state,
            AllocationState::Invalid,
            "only the matching active allocation is retired"
        );
        drop(reopened);
        let prepared = v2(real(&policy, 1, 17, 1));
        let restarted = restart_at(
            tmp.path(),
            &policy.text,
            &changed,
            &prepared,
            &[policy.fingerprint],
        )
        .expect("restart changed transcript");
        assert!(matches!(prepare_at(tmp.path(), policy.fingerprint, &policy.text, &restarted).expect("new attempt can fetch"), Signing::Missing),
            "fresh attempt reaches on-demand fetch instead of returning the retired transcript error");
    }

    #[test]
    fn indexes_include_plain_foreign_and_each_aggregate_occurrence() {
        let (a, fingerprint) = key(1);
        let (b, _) = key(2);
        let (c, _) = key(3);
        let text = format!("tr({b}/<0;1>/*,{{pk(musig({b},{c})/<0;1>/*),{{pk({c}/<0;1>/*),pk(musig({a},{b})/<512;513>/*)}}}})");
        assert_eq!(
            owned_expressions(&descriptor(&text).expect("policy"), fingerprint).expect("owned"),
            vec![3],
            "foreign internal, foreign aggregate, and plain leaf each occupy an index"
        );
        let duplicate_owner = format!("tr(musig({a},{a})/<0;1>/*)");
        assert!(
            owned_expressions(&descriptor(&duplicate_owner).expect("parse"), fingerprint).is_err(),
            "firmware cannot sign two owned participants in the same aggregate"
        );
    }

    #[test]
    fn unsupported_coverage_refuses_without_consuming_available_session() {
        let tmp = TempDir::new().expect("temporary cache");
        let (a, fingerprint) = key(1);
        let (b, _) = key(2);
        let policy = Policy {
            text: format!("tr(musig({a}/<0;1>/*,{b}/<0;1>/*))"),
            fingerprint,
        };
        let mut fetch =
            Fetch::begin_at(tmp.path(), fingerprint, &policy.text, 1).expect("fetch index zero");
        assert!(fetch.accept(&answer(&fetch, 1)).expect("fetch completed"));
        drop(fetch);
        assert!(prepare_at(tmp.path(), fingerprint, &policy.text, &v2(real(&policy, 1, 17, 1))).is_err(),
            "derive-then-aggregate changes the participant; do not inject another participant's nonce");
        let reopened = LockedCache::open(tmp.path(), fingerprint).expect("cache");
        assert_eq!(
            reopened.state.available.len(),
            1,
            "coverage failure leaves availability unchanged"
        );
        assert!(reopened.state.allocated.is_empty(), "no invalid allocation");
        drop(reopened);
        for quantity in [0, 9] {
            assert!(
                Fetch::begin_at(tmp.path(), fingerprint, &policy.text, quantity).is_err(),
                "invalid fetch quantity {quantity}"
            );
        }
        let parsed = descriptor(&policy.text).expect("policy");
        let indexes: BTreeSet<_> = owned_expressions(&parsed, fingerprint)
            .expect("indexes")
            .into_iter()
            .collect();
        assert_eq!(
            indexes,
            BTreeSet::from([0]),
            "whole aggregate is one expression"
        );
    }

    #[test]
    fn eight_command_batch_is_committed_only_after_the_eighth_success() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = policy(false, 0);
        let mut fetch = Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 8)
            .expect("maximum batch");
        assert!(!fetch.accept(&answer(&fetch, 1)).expect("command one"));
        assert!(!fetch.accept(&answer(&fetch, 2)).expect("command two"));
        assert!(!fetch.accept(&answer(&fetch, 3)).expect("command three"));
        assert!(!fetch.accept(&answer(&fetch, 4)).expect("command four"));
        assert!(!fetch.accept(&answer(&fetch, 5)).expect("command five"));
        assert!(!fetch.accept(&answer(&fetch, 6)).expect("command six"));
        assert!(!fetch.accept(&answer(&fetch, 7)).expect("command seven"));
        assert!(
            fetch.store.state.available.is_empty(),
            "seven successes are not a complete batch"
        );
        assert!(fetch.accept(&answer(&fetch, 8)).expect("command eight"));
        assert_eq!(
            fetch.store.state.available.len(),
            8,
            "full batch available together"
        );
        assert_eq!(
            fetch.store.state.fetch_order, 8,
            "every command has a durable order"
        );
        assert!(
            fetch.accept(&answer(&fetch, 8)).is_err(),
            "completed fetch cannot be reused"
        );
        drop(fetch);
        let mut oversized = real(&policy, 0, 9, MAX_INPUT);
        oversized.inputs.push(oversized.inputs[0].clone());
        oversized
            .unsigned_tx
            .input
            .push(oversized.unsigned_tx.input[0].clone());
        let error = match prepare_at(tmp.path(), policy.fingerprint, &policy.text, &v2(oversized)) {
            Err(error) => error,
            Ok(_) => panic!("nine inputs must fail"),
        };
        assert_eq!(
            error.to_string(),
            "Ledger nonce cache supports at most 8 inputs",
            "explicit input coverage failure"
        );
        let reopened = LockedCache::open(tmp.path(), policy.fingerprint).expect("reopen");
        assert_eq!(
            reopened.state.available.len(),
            8,
            "unsupported request consumes nothing"
        );
    }

    #[test]
    fn round_two_requires_every_owned_partial_signature_before_replacement() {
        let policy = policy(true, 0);
        let parsed = descriptor(&policy.text).expect("policy");
        let mut real = real(&policy, 1, 17, 1);
        assert!(
            check_round2(&real, &parsed, policy.fingerprint).is_err(),
            "nonce-only answer is not round-two success"
        );
        let slots = own_slots(&real, &parsed, policy.fingerprint).expect("owned slots");
        assert_eq!(slots.len(), 3, "one internal and two leaf expressions");
        for slot in &slots[..2] {
            set_musig_input_partial_sig(
                &mut real.inputs[0],
                &MusigPartialSig {
                    participant: slot.participant,
                    aggregate: slot.aggregate,
                    leaf_hash: slot.leaf_hash,
                    signature: [7; 32],
                },
            );
        }
        assert!(
            check_round2(&real, &parsed, policy.fingerprint).is_err(),
            "one missing leaf signature prevents replacement"
        );
        let slot = slots[2];
        set_musig_input_partial_sig(
            &mut real.inputs[0],
            &MusigPartialSig {
                participant: slot.participant,
                aggregate: slot.aggregate,
                leaf_hash: slot.leaf_hash,
                signature: [7; 32],
            },
        );
        assert!(
            check_round2(&real, &parsed, policy.fingerprint).is_ok(),
            "all device contributions received"
        );
    }

    #[test]
    fn repeated_aggregate_occurrences_do_not_advance_an_incomplete_nonce_round() {
        let tmp = TempDir::new().expect("temporary cache");
        let policy = policy(true, 0);
        let parsed = descriptor(&policy.text).expect("policy");
        let mut fetch =
            Fetch::begin_at(tmp.path(), policy.fingerprint, &policy.text, 1).expect("fetch");
        assert!(fetch.accept(&answer(&fetch, 1)).expect("fetch completed"));
        drop(fetch);
        let published = publish(tmp.path(), &policy, real(&policy, 1, 17, 1));
        let mut real = PsbtV2::deserialize(&published)
            .expect("v2")
            .into_bitcoin_psbt()
            .expect("v0");
        for seed in [2, 3] {
            let (_, fingerprint) = key(seed);
            for slot in own_slots(&real, &parsed, fingerprint).expect("peer slots") {
                set_musig_input_pub_nonce(
                    &mut real.inputs[0],
                    &MusigPubNonce {
                        participant: slot.participant,
                        aggregate: slot.aggregate,
                        leaf_hash: slot.leaf_hash,
                        nonce: nonce(seed + 5, 0, 0),
                    },
                );
            }
        }
        let owned = own_slots(&real, &parsed, policy.fingerprint).expect("owned slots");
        let omitted = owned[2];
        let fields = musig_input_pub_nonces(&real.inputs[0]).expect("complete fields");
        remove_musig_input_pub_nonces(&mut real.inputs[0]);
        for field in fields {
            if field.participant != omitted.participant
                || field.aggregate != omitted.aggregate
                || field.leaf_hash != omitted.leaf_hash
            {
                set_musig_input_pub_nonce(&mut real.inputs[0], &field);
            }
        }
        let status = signing_status(policy.fingerprint, &policy.text, &real)
            .expect("descriptor-aware status")
            .expect("owned policy");
        assert_eq!(
            status.round,
            MusigRound::Nonces,
            "two nonce fields cannot cover three expression occurrences"
        );
        let own = status
            .cosigners
            .iter()
            .find(|signer| signer.fingerprint == policy.fingerprint)
            .expect("signer status");
        assert_eq!(
            (own.nonces, own.expected),
            (2, 3),
            "picker must leave this Ledger eligible for round one"
        );
        match prepare_at(tmp.path(), policy.fingerprint, &policy.text, &v2(real))
            .expect("retry missing nonce")
        {
            Signing::Nonces(bytes) => {
                let restored = PsbtV2::deserialize(&bytes).expect("restored PSBT");
                assert_eq!(
                    musig_input_pub_nonces(&restored.inputs[0].psbt)
                        .expect("fields")
                        .len(),
                    6,
                    "all three owned and three peer nonces remain in round-one answer"
                );
            }
            _ => panic!("must publish cached nonces without a round-two device payload"),
        }
    }
}
