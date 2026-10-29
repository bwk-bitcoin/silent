//! QR protocol adapter for the shared MuSig session cache: transaction-independent
//! cache preparation, readiness, and allocation at the first signature request.
//!
//! Records are public-only by construction: a QR peer keeps its own secrets, so a
//! cache record never carries more than the coverage it asked for and the public
//! nonces the peer answered with.

use std::path::Path;

use bitcoin::{bip32::Fingerprint, hashes::Hash, secp256k1::PublicKey, Psbt};
use bwk::{
    bwk_descriptor::wallet::WalletDescriptorExt,
    bwk_sign::{
        bwk_musig::{nonce_agg, partial_verify, rust_bitcoin::RustBitcoin},
        musig::{expression_slots, own_slots, slot_infos, slot_sighash, MusigSlot, SlotInfo},
    },
    bwk_tx::bwk_psbt::musig::{
        musig_input_partial_sig, musig_input_pub_nonce, set_musig_input_partial_sig,
        set_musig_input_pub_nonce, MusigPartialSig,
    },
};
use bwk_qr_protocol::{request, response, MAX_MUSIG_DERIVATIONS};

use crate::{
    hwi,
    keys::{
        musig_sessions::{
            binding, descriptor, Allocation, AllocationState, LockedCache, PublishedNonce,
            QrDerivation, QrPreparation, QrStoredNonce, Session, SigningError, MAX_INPUT,
        },
        record::RouteKind,
    },
};

/// The addresses per policy path a preparation asks public nonces for. A session
/// never covers more, and allocation refuses transactions beyond the coverage.
pub(crate) const QR_ADDRESS_COVERAGE: u32 = 8;

fn lost(message: &str) -> SigningError {
    SigningError::SessionLost(message.to_owned())
}

pub fn prepare_material(
    fingerprint: Fingerprint,
    policy_text: &str,
) -> Result<request::PrepareMusig, String> {
    prepare_material_at(
        &crate::config::datadir().join("musig-sessions"),
        fingerprint,
        policy_text,
    )
}

/// Persists the preparation record under a fresh session id, or reuses the matching
/// in-flight one, and returns the request to show. The record is durable before any
/// bytes leave, so a lost exchange never takes a session.
pub(crate) fn prepare_material_at(
    root: &Path,
    fingerprint: Fingerprint,
    policy_text: &str,
) -> Result<request::PrepareMusig, String> {
    let parsed = descriptor(policy_text)?;
    let derivations = coverage(&parsed)?;
    let mut store = LockedCache::open(root, fingerprint)?;
    let session_id = match store
        .state
        .available
        .iter()
        .filter(|session| {
            session.provider == RouteKind::Qr
                && session.policy == policy_text
                && session
                    .qr
                    .as_ref()
                    .is_some_and(|qr| qr.derivations == derivations)
        })
        .min_by_key(|session| session.order)
    {
        Some(session) => session.id,
        None => {
            let mut id = [0; 32];
            getrandom::getrandom(&mut id).map_err(|e| e.to_string())?;
            let session = Session {
                provider: RouteKind::Qr,
                id,
                order: store.reserve_order()?,
                policy: policy_text.to_owned(),
                nonces: Vec::new(),
                fetched_for: None,
                secret: None,
                qr: Some(QrPreparation {
                    max_inputs: MAX_INPUT as u8,
                    derivations: derivations.clone(),
                    nonces: Vec::new(),
                }),
            };
            store.insert(session)?;
            id
        }
    };
    Ok(request::PrepareMusig {
        descriptor: request::Descriptor {
            alias: hwi::wallet_name(policy_text),
            descriptor: Some(request::DescriptorBody::Bip380(policy_text.to_owned())),
            proof: None,
        },
        session_id,
        max_inputs: MAX_INPUT as u8,
        derivations: derivations
            .into_iter()
            .map(|derivation| request::MusigDerivation {
                keychain: derivation.keychain,
                index: derivation.index,
            })
            .collect(),
    })
}

/// The finite public coverage every preparation asks for: every policy path at the
/// first `QR_ADDRESS_COVERAGE` addresses, inside the codec's derivation bound.
pub(crate) fn coverage(
    policy: &bwk::bwk_descriptor::wallet::WalletDescriptor,
) -> Result<Vec<QrDerivation>, String> {
    let paths = policy.single_paths().map_err(|e| e.to_string())?;
    let mut derivations = Vec::new();
    for keychain in 0..paths.len() as u32 {
        for index in 0..QR_ADDRESS_COVERAGE {
            derivations.push(QrDerivation { keychain, index });
        }
    }
    if derivations.len() > MAX_MUSIG_DERIVATIONS {
        return Err(format!(
            "policy has {} paths, the QR cache covers at most {} derivations",
            paths.len(),
            MAX_MUSIG_DERIVATIONS / QR_ADDRESS_COVERAGE as usize
        ));
    }
    Ok(derivations)
}

pub fn accept_prepared(
    fingerprint: Fingerprint,
    policy_text: &str,
    prepared: response::MusigPrepared,
) -> Result<(), String> {
    accept_prepared_at(
        &crate::config::datadir().join("musig-sessions"),
        fingerprint,
        policy_text,
        prepared,
    )
}

/// Stores the answered nonces on the preparation they belong to. The response is
/// checked one-to-one against the slots the asked coverage derives from the policy,
/// so a peer cannot relabel a nonce for another key or slot.
pub(crate) fn accept_prepared_at(
    root: &Path,
    fingerprint: Fingerprint,
    policy_text: &str,
    prepared: response::MusigPrepared,
) -> Result<(), String> {
    let parsed = descriptor(policy_text)?;
    prepared
        .validate()
        .map_err(|e| format!("prepared coverage outside the codec bounds: {e}"))?;
    let mut store = LockedCache::open(root, fingerprint)?;
    let index = store
        .state
        .available
        .iter()
        .position(|session| {
            session.provider == RouteKind::Qr
                && session.policy == policy_text
                && session.id == prepared.session_id
        })
        .ok_or("no preparation in flight for this session")?;
    let record = store.state.available[index]
        .qr
        .as_ref()
        .ok_or("stored session has no QR coverage")?;
    if prepared.max_inputs != record.max_inputs {
        return Err("prepared coverage differs from the request".into());
    }
    let expected = expected_slots(&parsed, fingerprint, &record.derivations, record.max_inputs)?;
    if prepared.nonces.len() != expected.len() {
        return Err("prepared nonce count does not match the coverage".into());
    }
    let mut stored = Vec::with_capacity(prepared.nonces.len());
    for (derivation, slot, expression) in &expected {
        let matches: Vec<_> = prepared
            .nonces
            .iter()
            .filter(|nonce| {
                nonce.derivation.keychain == derivation.keychain
                    && nonce.derivation.index == derivation.index
                    && nonce.input_index == slot.input_index as u32
                    && nonce.expression_index == *expression as u32
                    && nonce.participant.0 == slot.participant.serialize()
                    && nonce.aggregate.0 == slot.aggregate.serialize()
                    && nonce.leaf_hash == slot.leaf_hash.map(|hash| hash.to_byte_array())
            })
            .collect();
        let [nonce] = matches.as_slice() else {
            return Err("wrong or duplicate prepared nonce attribution".into());
        };
        valid_nonce(&nonce.nonce)?;
        stored.push(QrStoredNonce {
            derivation: *derivation,
            input: nonce.input_index,
            expression: nonce.expression_index,
            participant: slot.participant,
            aggregate: slot.aggregate,
            leaf_hash: slot.leaf_hash,
            bytes: nonce.nonce.to_vec(),
        });
    }
    let mut next = store.state.clone();
    let prepared_record = next.available[index]
        .qr
        .as_mut()
        .ok_or("stored session has no QR coverage")?;
    if !prepared_record.nonces.is_empty() {
        if prepared_record.nonces != stored {
            return Err("prepared public nonces changed".into());
        }
        return Ok(());
    }
    prepared_record.nonces = stored;
    store.save(next)
}

/// Every slot the asked coverage owns, as (derivation, slot, expression index).
fn expected_slots(
    policy: &bwk::bwk_descriptor::wallet::WalletDescriptor,
    fingerprint: Fingerprint,
    derivations: &[QrDerivation],
    max_inputs: u8,
) -> Result<Vec<(QrDerivation, MusigSlot, usize)>, String> {
    let paths = policy.single_paths().map_err(|e| e.to_string())?;
    let mut expected = Vec::new();
    for derivation in derivations {
        let single = paths
            .get(derivation.keychain as usize)
            .ok_or("prepared coverage names an unknown policy path")?;
        let definite = single
            .derive_at(derivation.index)
            .map_err(|e| e.to_string())?;
        for input in 0..u32::from(max_inputs) {
            for info in expression_slots(input as usize, &definite).map_err(|e| e.to_string())? {
                if info.signer.0 != fingerprint {
                    continue;
                }
                expected.push((*derivation, info.slot, info.expression_index));
            }
        }
    }
    Ok(expected)
}

fn valid_nonce(nonce: &[u8; 66]) -> Result<(), String> {
    PublicKey::from_slice(&nonce[..33]).map_err(|_| "invalid prepared public nonce".to_string())?;
    PublicKey::from_slice(&nonce[33..]).map_err(|_| "invalid prepared public nonce".to_string())?;
    Ok(())
}

/// Whether a usable prepared session or an active allocation exists for this
/// transaction, read from the in-memory snapshot only.
pub fn qr_ready(fingerprint: Fingerprint, policy_text: &str, real: &Psbt) -> Result<bool, String> {
    let parsed = descriptor(policy_text)?;
    let binding = binding(real, &parsed, fingerprint)?;
    let Some(state) = crate::keys::musig_sessions::snapshot(fingerprint) else {
        return Ok(false);
    };
    if state.is_retired(binding, RouteKind::Qr) {
        return Ok(false);
    }
    if state
        .allocated
        .iter()
        .any(|allocation| allocation.binding == binding && allocation.provider == RouteKind::Qr)
    {
        return Ok(true);
    }
    let slots = own_slots(real, &parsed, fingerprint).map_err(|e| e.to_string())?;
    if slots.is_empty() {
        return Ok(true);
    }
    Ok(state.available.iter().any(|session| {
        session.provider == RouteKind::Qr
            && session.policy == policy_text
            && session
                .qr
                .as_ref()
                .is_some_and(|qr| covers(&qr.nonces, &slots))
    }))
}

fn covers(nonces: &[QrStoredNonce], slots: &[MusigSlot]) -> bool {
    slots.iter().all(|slot| {
        nonces.iter().any(|nonce| {
            nonce.participant == slot.participant
                && nonce.aggregate == slot.aggregate
                && nonce.leaf_hash == slot.leaf_hash
        })
    })
}

pub fn allocate(
    fingerprint: Fingerprint,
    policy_text: &str,
    real: &mut Psbt,
) -> Result<[u8; 32], SigningError> {
    allocate_at(
        &crate::config::datadir().join("musig-sessions"),
        fingerprint,
        policy_text,
        real,
    )
}

/// Takes the oldest prepared session covering the transaction for this attempt and
/// inserts its nonces. Nothing is visible until the allocation is durable, and a
/// taken session never returns to availability. Idempotent on the active allocation.
pub(crate) fn allocate_at(
    root: &Path,
    fingerprint: Fingerprint,
    policy_text: &str,
    real: &mut Psbt,
) -> Result<[u8; 32], SigningError> {
    let parsed = descriptor(policy_text).map_err(|e| e.to_string())?;
    let infos = own_infos(real, &parsed, fingerprint)?;
    let binding = binding(real, &parsed, fingerprint).map_err(|e| e.to_string())?;
    let mut store = LockedCache::open(root, fingerprint).map_err(|e| e.to_string())?;
    if let Some(allocation) = store.active(binding, policy_text, RouteKind::Qr)? {
        if !allocation.covers_slots(&slot_set(&infos)) {
            return Err(lost("MuSig slots changed after cache allocation"));
        }
        if !allocation.compatible_nonces(real, true)? {
            return Err(lost("cached nonces do not match the transaction"));
        }
        return Ok(allocation.id);
    }
    let slots: Vec<MusigSlot> = infos.iter().map(|info| info.slot).collect();
    for info in &infos {
        if musig_input_partial_sig(
            &real.inputs[info.slot.input_index],
            info.slot.participant,
            info.slot.aggregate,
            info.slot.leaf_hash,
        )
        .map_err(|e| e.to_string())?
        .is_some()
        {
            return Err(lost("own partial signature is already present"));
        }
    }
    let index = store
        .state
        .available
        .iter()
        .enumerate()
        .filter(|(_, session)| {
            session.provider == RouteKind::Qr
                && session.policy == policy_text
                && session
                    .qr
                    .as_ref()
                    .is_some_and(|qr| covers(&qr.nonces, &slots))
        })
        .min_by_key(|(_, session)| session.order)
        .map(|(index, _)| index)
        .ok_or_else(|| lost("no prepared QR cache session covers this transaction"))?;
    let mut next = store.state.clone();
    let session = next.available.remove(index);
    let qr = session
        .qr
        .as_ref()
        .ok_or_else(|| lost("stored session has no QR coverage"))?;
    let mut published = Vec::new();
    for info in &infos {
        let nonce = qr
            .nonces
            .iter()
            .find(|nonce| {
                nonce.participant == info.slot.participant
                    && nonce.aggregate == info.slot.aggregate
                    && nonce.leaf_hash == info.slot.leaf_hash
            })
            .ok_or_else(|| lost("prepared session does not cover this transaction"))?;
        published.push(PublishedNonce {
            input: info.slot.input_index,
            participant: info.slot.participant,
            aggregate: info.slot.aggregate,
            leaf_hash: info.slot.leaf_hash,
            bytes: nonce.bytes.clone(),
        });
    }
    let mut published_psbt = real.clone();
    for entry in &published {
        set_musig_input_pub_nonce(
            &mut published_psbt.inputs[entry.input],
            &entry.field().map_err(|e| e.to_string())?,
        );
    }
    let allocation =
        store.allocate_transcript(next, session, binding, policy_text, published, None)?;
    *real = published_psbt;
    Ok(allocation.id)
}

pub fn pin_transcript(
    fingerprint: Fingerprint,
    policy_text: &str,
    real: &Psbt,
) -> Result<(), SigningError> {
    pin_transcript_at(
        &crate::config::datadir().join("musig-sessions"),
        fingerprint,
        policy_text,
        real,
    )
}

/// Pins the participant nonces of every own slot once every required signer's nonces
/// are in: the transcript the signing answers are held to.
pub(crate) fn pin_transcript_at(
    root: &Path,
    fingerprint: Fingerprint,
    policy_text: &str,
    real: &Psbt,
) -> Result<(), SigningError> {
    let parsed = descriptor(policy_text).map_err(|e| e.to_string())?;
    let infos = own_infos(real, &parsed, fingerprint)?;
    let binding = binding(real, &parsed, fingerprint).map_err(|e| e.to_string())?;
    let transcript = pinned_transcript(real, &infos)?;
    let mut store = LockedCache::open(root, fingerprint).map_err(|e| e.to_string())?;
    let mut next = store.state.clone();
    let allocation = next
        .allocated
        .iter_mut()
        .find(|allocation| {
            allocation.provider == RouteKind::Qr
                && allocation.binding == binding
                && allocation.state == AllocationState::Active
        })
        .ok_or_else(|| lost("no active cache allocation for this attempt"))?;
    if allocation.qr_transcript.is_some() {
        return Ok(());
    }
    allocation.qr_transcript = Some(transcript);
    store.save(next).map_err(SigningError::from)
}

pub fn accept_signed(
    fingerprint: Fingerprint,
    policy_text: &str,
    original: &Psbt,
    returned: &Psbt,
) -> Result<(), SigningError> {
    accept_signed_at(
        &crate::config::datadir().join("musig-sessions"),
        fingerprint,
        policy_text,
        original,
        returned,
    )
}

/// Validates a signing answer against the allocation and consumes it: the own
/// nonces and the pinned transcript must come back untouched, the only allowed
/// change is the own partial signature, and that signature must verify against the
/// allocated nonce. Consumed sessions never revive.
pub(crate) fn accept_signed_at(
    root: &Path,
    fingerprint: Fingerprint,
    policy_text: &str,
    original: &Psbt,
    returned: &Psbt,
) -> Result<(), SigningError> {
    let parsed = descriptor(policy_text).map_err(|e| e.to_string())?;
    if original.inputs.len() != returned.inputs.len() {
        return Err("returned input count changed".into());
    }
    let binding = binding(original, &parsed, fingerprint).map_err(|e| e.to_string())?;
    let mut store = LockedCache::open(root, fingerprint).map_err(|e| e.to_string())?;
    let index = store
        .state
        .allocated
        .iter()
        .position(|allocation| {
            allocation.provider == RouteKind::Qr && allocation.binding == binding
        })
        .ok_or_else(|| lost("no cache allocation for this signing attempt"))?;
    let allocation = store.state.allocated[index].clone();
    if allocation.state != AllocationState::Active {
        return Err(lost("cached signing attempt is no longer active"));
    }
    if allocation.policy != policy_text {
        return Err(lost("cache allocation belongs to another policy"));
    }
    if !allocation.compatible_nonces(returned, true)? {
        return Err(lost("returned nonces do not match the cache allocation"));
    }
    let infos = own_infos(original, &parsed, fingerprint)?;
    let pinned = allocation
        .qr_transcript
        .clone()
        .ok_or_else(|| lost("cache allocation has no pinned transcript"))?;
    check_transcript(returned, &infos, &pinned)?;
    let mut expected = original.clone();
    let mut consumed = false;
    for info in &infos {
        let signature = musig_input_partial_sig(
            &returned.inputs[info.slot.input_index],
            info.slot.participant,
            info.slot.aggregate,
            info.slot.leaf_hash,
        )
        .map_err(|e| e.to_string())?;
        let Some(signature) = signature else {
            continue;
        };
        verify_partial_sig(original, info, &allocation, signature)?;
        set_musig_input_partial_sig(
            &mut expected.inputs[info.slot.input_index],
            &MusigPartialSig {
                participant: info.slot.participant,
                aggregate: info.slot.aggregate,
                leaf_hash: info.slot.leaf_hash,
                signature,
            },
        );
        consumed = true;
    }
    if !consumed {
        return Err("cached signing answer carries no own partial signature".into());
    }
    if expected != *returned {
        return Err("unexpected returned PSBT changes".into());
    }
    let mut next = store.state.clone();
    next.allocated[index].state = AllocationState::Consumed;
    store.save(next).map_err(SigningError::from)
}

fn verify_partial_sig(
    original: &Psbt,
    info: &SlotInfo,
    allocation: &Allocation,
    signature: [u8; 32],
) -> Result<(), SigningError> {
    let published = allocation
        .slots
        .iter()
        .find(|entry| entry.slot() == info.slot)
        .ok_or_else(|| lost("cache allocation does not cover this slot"))?;
    let own_nonce: [u8; 66] = published
        .bytes
        .as_slice()
        .try_into()
        .map_err(|_| lost("invalid cached public nonce length"))?;
    let key_agg = if info.slot.leaf_hash.is_none() {
        info.agg.key_path_key_agg(info.merkle_root)
    } else {
        info.agg.key_agg()
    }
    .map_err(|e| SigningError::Invalid(format!("cannot aggregate the policy key: {e}")))?;
    let input = &original.inputs[info.slot.input_index];
    let mut nonces = Vec::new();
    for participant in info.agg.participants() {
        nonces.push(
            musig_input_pub_nonce(
                input,
                *participant,
                info.slot.aggregate,
                info.slot.leaf_hash,
            )
            .map_err(|e| e.to_string())?
            .ok_or_else(|| lost("participant nonce is missing"))?,
        );
    }
    let aggregate_nonce = nonce_agg(&RustBitcoin, &nonces)
        .map_err(|e| SigningError::Invalid(format!("cannot aggregate nonces: {e}")))?;
    let sighash = slot_sighash(original, &info.slot).map_err(|e| e.to_string())?;
    if !partial_verify(
        &RustBitcoin,
        &signature,
        &own_nonce,
        &info.slot.participant.serialize(),
        &key_agg,
        &aggregate_nonce,
        &sighash,
    ) {
        return Err("invalid returned partial signature".into());
    }
    Ok(())
}

fn own_infos(
    real: &Psbt,
    parsed: &bwk::bwk_descriptor::wallet::WalletDescriptor,
    fingerprint: Fingerprint,
) -> Result<Vec<SlotInfo>, SigningError> {
    let infos: Vec<SlotInfo> = slot_infos(real, parsed)
        .map_err(|e| e.to_string())?
        .into_iter()
        .filter(|info| info.signer.0 == fingerprint)
        .collect();
    if infos.is_empty() {
        return Err(lost("transaction requests no owned MuSig slots"));
    }
    Ok(infos)
}

fn slot_set(infos: &[SlotInfo]) -> Vec<MusigSlot> {
    infos.iter().map(|info| info.slot).collect()
}

/// One group of concatenated 66-byte participant nonces per own slot, in slot order.
fn pinned_transcript(real: &Psbt, infos: &[SlotInfo]) -> Result<Vec<Vec<u8>>, SigningError> {
    let mut transcript = Vec::new();
    for info in infos {
        let input = &real.inputs[info.slot.input_index];
        let mut group = Vec::new();
        for participant in info.agg.participants() {
            let nonce: [u8; 66] = musig_input_pub_nonce(
                input,
                *participant,
                info.slot.aggregate,
                info.slot.leaf_hash,
            )
            .map_err(|e| e.to_string())?
            .ok_or_else(|| lost("participant nonce is missing for the cached transcript"))?;
            group.extend_from_slice(&nonce);
        }
        transcript.push(group);
    }
    Ok(transcript)
}

/// The returned PSBT must still carry exactly the pinned participant nonces.
fn check_transcript(
    returned: &Psbt,
    infos: &[SlotInfo],
    pinned: &[Vec<u8>],
) -> Result<(), SigningError> {
    if infos.len() != pinned.len() {
        return Err(lost(
            "cache allocation does not match the transaction slots",
        ));
    }
    for (info, group) in infos.iter().zip(pinned.iter()) {
        if group.len() != info.agg.participants().len() * 66 || group.len() % 66 != 0 {
            return Err(lost("returned participant transcript changed"));
        }
        let input = &returned.inputs[info.slot.input_index];
        for (participant, nonce) in info.agg.participants().iter().zip(group.chunks_exact(66)) {
            let pinned_nonce: [u8; 66] = nonce
                .try_into()
                .map_err(|_| lost("invalid pinned nonce length"))?;
            let found = musig_input_pub_nonce(
                input,
                *participant,
                info.slot.aggregate,
                info.slot.leaf_hash,
            )
            .map_err(|e| e.to_string())?;
            if found != Some(pinned_nonce) {
                return Err(lost("returned participant transcript changed"));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::str::FromStr;

    use bitcoin::{
        absolute::LockTime,
        bip32::{DerivationPath, Fingerprint},
        hashes::Hash,
        transaction::Version,
        Amount, Network, OutPoint, Psbt, ScriptBuf, Sequence, Transaction, TxIn, TxOut,
    };
    use bwk::{
        bwk_descriptor::{updater::update_input, wallet::WalletDescriptorExt},
        bwk_sign::{
            hot_musig::HotMusigSession,
            hot_signer::HotSigner,
            musig::{expression_slots, MusigSessionId},
        },
    };
    use bwk_qr_protocol::{request, response, MAX_MUSIG_DERIVATIONS};
    use temp_dir::TempDir;

    use crate::keys::musig_sessions::{
        descriptor,
        qr_provider::{
            accept_prepared_at, accept_signed_at, allocate_at, coverage, pin_transcript_at,
            prepare_material_at, qr_ready,
        },
        AllocationState, LockedCache, SigningError, MAX_INPUT,
    };
    use crate::keys::record::RouteKind;

    const MNEMONIC_A: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    const MNEMONIC_B: &str =
        "legal winner thank year wave sausage worth useful legal winner thank yellow";

    struct Fixture {
        root: TempDir,
        a: HotSigner,
        b: HotSigner,
        fingerprint: Fingerprint,
        policy: String,
    }

    fn fixture() -> Fixture {
        let a = HotSigner::new_from_mnemonics(Network::Regtest, MNEMONIC_A).expect("signer a");
        let b = HotSigner::new_from_mnemonics(Network::Regtest, MNEMONIC_B).expect("signer b");
        let path = DerivationPath::from_str("m/87'/1'/0'").expect("account path");
        let policy = format!(
            "tr(musig([{}/87h/1h/0h]{}/<0;1>/*,[{}/87h/1h/0h]{}/<0;1>/*))",
            a.fingerprint(),
            a.xpub(&path).xkey,
            b.fingerprint(),
            b.xpub(&path).xkey
        );
        let fingerprint = a.fingerprint();
        Fixture {
            root: TempDir::new().expect("temp directory"),
            a,
            b,
            fingerprint,
            policy,
        }
    }

    impl Fixture {
        fn root(&self) -> &std::path::Path {
            self.root.path()
        }

        fn parsed(&self) -> bwk::bwk_descriptor::wallet::WalletDescriptor {
            descriptor(&self.policy).expect("policy")
        }
    }

    fn transaction(policy: &str, index: u32) -> Psbt {
        let parsed = descriptor(policy).expect("policy");
        let definite = parsed.single_paths().expect("paths")[0]
            .derive_at(index)
            .expect("derive address");
        let tx = Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: vec![TxIn {
                previous_output: OutPoint::null(),
                script_sig: ScriptBuf::new(),
                sequence: Sequence::MAX,
                witness: Default::default(),
            }],
            output: vec![TxOut {
                value: Amount::from_sat(90_000),
                script_pubkey: definite.script_pubkey(),
            }],
        };
        let mut psbt = Psbt::from_unsigned_tx(tx).expect("PSBT");
        psbt.inputs[0].witness_utxo = Some(TxOut {
            value: Amount::from_sat(100_000),
            script_pubkey: definite.script_pubkey(),
        });
        update_input(&mut psbt, 0, &definite).expect("input metadata");
        psbt
    }

    /// The prepared answer a faithful peer sends for the asked coverage: every owned
    /// slot of every covered derivation, with the device session's real nonces.
    fn prepared_answer(
        session: &HotMusigSession,
        fixture: &Fixture,
        session_id: [u8; 32],
    ) -> response::MusigPrepared {
        let parsed = fixture.parsed();
        let derivations = coverage(&parsed).expect("coverage");
        let mut nonces = Vec::new();
        for derivation in &derivations {
            let definite = parsed.single_paths().expect("paths")[derivation.keychain as usize]
                .derive_at(derivation.index)
                .expect("derive");
            let advertised = session
                .public_nonces(&parsed, derivation.keychain as usize, derivation.index)
                .expect("device nonces");
            for input in 0..MAX_INPUT {
                for info in expression_slots(input, &definite).expect("expression slots") {
                    if info.signer.0 != fixture.fingerprint {
                        continue;
                    }
                    let entry = advertised
                        .iter()
                        .find(|entry| {
                            entry.input_index == input
                                && entry.expression_index == info.expression_index
                        })
                        .expect("device nonce for the slot");
                    nonces.push(response::MusigNonce {
                        derivation: request::MusigDerivation {
                            keychain: derivation.keychain,
                            index: derivation.index,
                        },
                        input_index: input as u32,
                        expression_index: info.expression_index as u32,
                        participant: bwk_qr_protocol::types::PublicKey(
                            entry.nonce.participant.serialize(),
                        ),
                        aggregate: bwk_qr_protocol::types::PublicKey(
                            info.slot.aggregate.serialize(),
                        ),
                        leaf_hash: info.slot.leaf_hash.map(|hash| hash.to_byte_array()),
                        nonce: entry.nonce.nonce,
                    });
                }
            }
        }
        response::MusigPrepared {
            session_id,
            max_inputs: MAX_INPUT as u8,
            nonces,
        }
    }

    fn device_session(device: &HotSigner, fixture: &Fixture, id: [u8; 32]) -> HotMusigSession {
        device
            .create_musig_session(&fixture.parsed(), MusigSessionId(id), MAX_INPUT as u8)
            .expect("device session")
    }

    fn prepare(fixture: &Fixture) -> request::PrepareMusig {
        prepare_material_at(fixture.root(), fixture.fingerprint, &fixture.policy)
            .expect("prepare material")
    }

    fn accept_prepared_ok(fixture: &Fixture, session: &HotMusigSession, session_id: [u8; 32]) {
        let answer = prepared_answer(session, fixture, session_id);
        accept_prepared_at(fixture.root(), fixture.fingerprint, &fixture.policy, answer)
            .expect("accept prepared");
    }

    #[test]
    fn prepare_persists_a_public_record_and_reuses_its_session_id() {
        let fixture = fixture();
        let preparation = prepare(&fixture);
        assert_eq!(preparation.max_inputs, MAX_INPUT as u8, "input coverage");
        assert!(
            !preparation.derivations.is_empty()
                && preparation.derivations.len() <= MAX_MUSIG_DERIVATIONS,
            "coverage inside the codec bound"
        );
        assert_eq!(
            preparation.derivations[0],
            request::MusigDerivation {
                keychain: 0,
                index: 0
            },
            "coverage starts at the first address"
        );
        let store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("open cache");
        assert_eq!(
            store.state.available.len(),
            1,
            "one preparation record persisted"
        );
        let session = &store.state.available[0];
        assert_eq!(session.id, preparation.session_id, "session id carried");
        assert_eq!(session.provider, RouteKind::Qr, "QR provider record");
        assert!(
            session.secret.is_none() && session.fetched_for.is_none(),
            "no secret material and no transaction binding on a QR record"
        );
        assert_eq!(
            session.qr.as_ref().expect("coverage").nonces.len(),
            0,
            "nothing stored before the answer"
        );
        assert!(
            !serde_json::to_string(&store.state)
                .expect("cache json")
                .contains("session root"),
            "record names no secret material"
        );
        drop(store);
        let again = prepare(&fixture);
        assert_eq!(
            again.session_id, preparation.session_id,
            "the in-flight preparation is reused, never duplicated"
        );
        assert_eq!(again.derivations, preparation.derivations);
    }

    #[test]
    fn accept_prepared_stores_only_matching_attribution_once() {
        let fixture = fixture();
        let session = device_session(&fixture.a, &fixture, [7; 32]);
        let preparation = prepare(&fixture);
        let session_id = preparation.session_id;

        let mut wrong = prepared_answer(&session, &fixture, session_id);
        wrong.session_id = [9; 32];
        assert!(
            accept_prepared_at(fixture.root(), fixture.fingerprint, &fixture.policy, wrong)
                .is_err(),
            "an answer for another session is refused"
        );

        let mut relabeled = prepared_answer(&session, &fixture, session_id);
        let peer_slot = relabeled
            .nonces
            .iter()
            .position(|nonce| nonce.participant.0 != [0; 33])
            .expect("own nonce");
        let last = relabeled.nonces.len() - 1;
        relabeled.nonces.swap(peer_slot, last);
        // Swapping keeps the set but breaks one nonce's attribution.
        relabeled.nonces[peer_slot].derivation.index = relabeled.nonces[last].derivation.index;
        assert!(
            accept_prepared_at(
                fixture.root(),
                fixture.fingerprint,
                &fixture.policy,
                relabeled
            )
            .is_err(),
            "a relabeled nonce is refused"
        );

        accept_prepared_ok(&fixture, &session, session_id);
        let second = accept_prepared_at(
            fixture.root(),
            fixture.fingerprint,
            &fixture.policy,
            prepared_answer(&session, &fixture, session_id),
        );
        assert!(
            second.is_ok(),
            "an identical re-acceptance is idempotent: {second:?}"
        );
        let store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("reopen");
        let prepared = store.state.available[0]
            .qr
            .as_ref()
            .expect("coverage stored");
        assert!(
            !prepared.nonces.is_empty(),
            "the answered nonces are on the record"
        );
        assert!(
            prepared.nonces.iter().all(|nonce| nonce.bytes.len() == 66),
            "only public nonce material is stored"
        );
    }

    #[test]
    fn accept_prepared_for_an_unknown_session_is_refused() {
        let fixture = fixture();
        let session = device_session(&fixture.a, &fixture, [5; 32]);
        let answer = prepared_answer(&session, &fixture, [5; 32]);
        let error =
            accept_prepared_at(fixture.root(), fixture.fingerprint, &fixture.policy, answer)
                .expect_err("no preparation is in flight");
        assert!(
            error.contains("no preparation in flight"),
            "a late answer without its record is refused, got {error}"
        );
    }

    #[test]
    fn qr_ready_reflects_coverage_and_allocation() {
        let fixture = fixture();
        let parsed = fixture.parsed();
        let tx = transaction(&fixture.policy, 0);
        assert!(
            !qr_ready(fixture.fingerprint, &fixture.policy, &tx).expect("empty readiness"),
            "an empty cache is not ready"
        );
        let session = device_session(&fixture.a, &fixture, [3; 32]);
        let preparation = prepare(&fixture);
        accept_prepared_ok(&fixture, &session, preparation.session_id);
        assert!(
            qr_ready(fixture.fingerprint, &fixture.policy, &tx).expect("prepared readiness"),
            "a prepared session covering the transaction is ready"
        );
        let outside = transaction(&fixture.policy, 90);
        assert!(
            !qr_ready(fixture.fingerprint, &fixture.policy, &outside).expect("coverage readiness"),
            "a transaction outside the finite coverage is not ready"
        );
        let mut allocated = tx.clone();
        allocate_at(
            fixture.root(),
            fixture.fingerprint,
            &fixture.policy,
            &mut allocated,
        )
        .expect("allocate");
        assert!(
            qr_ready(fixture.fingerprint, &fixture.policy, &tx).expect("allocated readiness"),
            "an active allocation keeps the attempt ready"
        );
        assert_eq!(
            parsed.single_paths().expect("paths").len(),
            2,
            "fixture sanity: both branches of the multipath are covered"
        );
    }

    #[test]
    fn allocation_is_durable_fifo_and_never_returns_to_available() {
        let fixture = fixture();
        // Two prepared sessions: the first one in flight, an older-ordered one behind.
        let first_session = device_session(&fixture.a, &fixture, [1; 32]);
        let preparation = prepare(&fixture);
        accept_prepared_ok(&fixture, &first_session, preparation.session_id);
        {
            let mut store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("open");
            let mut next = store.state.clone();
            let mut second = next.available[0].clone();
            second.id = [2; 32];
            second.order = 0;
            next.available.insert(0, second);
            store.save(next).expect("prepend an older session");
        }
        let mut tx = transaction(&fixture.policy, 0);
        let session_id = allocate_at(
            fixture.root(),
            fixture.fingerprint,
            &fixture.policy,
            &mut tx,
        )
        .expect("allocate");
        assert_eq!(
            session_id, [2; 32],
            "the oldest session is taken, whatever its id"
        );
        let store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("reopen");
        assert_eq!(
            store.state.allocated.len(),
            1,
            "exactly one allocation is on disk"
        );
        assert_eq!(store.state.allocated[0].id, [2; 32]);
        assert_eq!(
            store.state.allocated[0].provider,
            RouteKind::Qr,
            "the allocation belongs to the QR provider"
        );
        assert!(
            store.state.allocated[0].secret.is_none(),
            "a QR allocation never carries secret material"
        );
        assert!(
            store.state.allocated[0].qr_transcript.is_none(),
            "the transcript is pinned separately, after every signer is in"
        );
        assert_eq!(
            store.state.available.len(),
            1,
            "only the remaining session stays available"
        );
        drop(store);
        let retry = allocate_at(
            fixture.root(),
            fixture.fingerprint,
            &fixture.policy,
            &mut tx,
        )
        .expect("retry");
        assert_eq!(retry, session_id, "a retry reuses the active allocation");
        let store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("reopen");
        assert_eq!(
            store.state.available.len(),
            1,
            "a retry never takes another session"
        );
    }

    #[test]
    fn allocation_refuses_uncovered_transactions() {
        let fixture = fixture();
        let session = device_session(&fixture.a, &fixture, [4; 32]);
        let preparation = prepare(&fixture);
        accept_prepared_ok(&fixture, &session, preparation.session_id);
        let mut outside = transaction(&fixture.policy, 90);
        let error = allocate_at(
            fixture.root(),
            fixture.fingerprint,
            &fixture.policy,
            &mut outside,
        )
        .expect_err("outside the coverage");
        assert!(
            matches!(error, SigningError::SessionLost(_)),
            "an uncovered transaction must not take a session, got {error}"
        );
        let store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("reopen");
        assert!(
            store.state.allocated.is_empty(),
            "nothing was allocated for an uncovered transaction"
        );
        assert_eq!(
            store.state.available.len(),
            1,
            "the session stayed available"
        );
    }

    #[test]
    fn pin_requires_every_participant_and_accept_signed_consumes() {
        let fixture = fixture();
        let session = device_session(&fixture.a, &fixture, [6; 32]);
        let preparation = prepare(&fixture);
        accept_prepared_ok(&fixture, &session, preparation.session_id);
        let mut tx = transaction(&fixture.policy, 0);
        allocate_at(
            fixture.root(),
            fixture.fingerprint,
            &fixture.policy,
            &mut tx,
        )
        .expect("allocate");

        // The peer's nonces are missing: the transcript cannot be pinned yet.
        assert!(
            pin_transcript_at(fixture.root(), fixture.fingerprint, &fixture.policy, &tx).is_err(),
            "an incomplete nonce set is not pinned"
        );
        let parsed = fixture.parsed();
        fixture
            .b
            .create_musig_session(&parsed, MusigSessionId([90; 32]), MAX_INPUT as u8)
            .expect("peer session")
            .publish_nonces(&mut tx, &parsed)
            .expect("peer nonces");
        pin_transcript_at(fixture.root(), fixture.fingerprint, &fixture.policy, &tx)
            .expect("pin once every participant is in");
        {
            let store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("reopen");
            assert!(
                store.state.allocated[0].qr_transcript.is_some(),
                "the pinned transcript is durable"
            );
        }

        // The device signs with the very session its prepared nonces came from.
        fixture
            .a
            .sign_musig_session(&mut tx, &parsed, session)
            .expect("device partial signature");
        accept_signed_at(
            fixture.root(),
            fixture.fingerprint,
            &fixture.policy,
            &tx,
            &tx,
        )
        .expect("accept the signing answer");
        let store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("reopen");
        assert_eq!(
            store.state.allocated[0].state,
            AllocationState::Consumed,
            "a verified answer consumes the allocation"
        );
        drop(store);
        assert!(
            accept_signed_at(
                fixture.root(),
                fixture.fingerprint,
                &fixture.policy,
                &tx,
                &tx
            )
            .is_err(),
            "a consumed allocation never signs again"
        );

        // A fresh attempt allocates again; a tampered answer is refused untouched.
        let session = device_session(&fixture.a, &fixture, [8; 32]);
        let preparation = prepare(&fixture);
        accept_prepared_ok(&fixture, &session, preparation.session_id);
        let mut tx = transaction(&fixture.policy, 0);
        crate::keys::musig_sessions::start_attempt(&mut tx).expect("a fresh attempt");
        allocate_at(
            fixture.root(),
            fixture.fingerprint,
            &fixture.policy,
            &mut tx,
        )
        .expect("allocate again");
        fixture
            .b
            .create_musig_session(&parsed, MusigSessionId([91; 32]), MAX_INPUT as u8)
            .expect("peer session")
            .publish_nonces(&mut tx, &parsed)
            .expect("peer nonces");
        pin_transcript_at(fixture.root(), fixture.fingerprint, &fixture.policy, &tx).expect("pin");
        let mut tampered = tx.clone();
        tampered.unsigned_tx.output[0].value = Amount::from_sat(89_999);
        assert!(
            accept_signed_at(
                fixture.root(),
                fixture.fingerprint,
                &fixture.policy,
                &tx,
                &tampered
            )
            .is_err(),
            "an answer changing the transaction is refused"
        );
        let store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("reopen");
        assert_eq!(
            store.state.allocated[1].state,
            AllocationState::Active,
            "a refused answer leaves the allocation active"
        );
    }

    #[test]
    fn allocate_first_request_binds_every_required_signer() {
        let fixture = fixture();
        let session = device_session(&fixture.a, &fixture, [10; 32]);
        let preparation = prepare(&fixture);
        accept_prepared_ok(&fixture, &session, preparation.session_id);
        // The peer's own cache: one hot session, ready for the same attempt.
        {
            let mut peer_store =
                LockedCache::open(fixture.root(), fixture.b.fingerprint()).expect("peer cache");
            crate::keys::musig_sessions::hot_provider::create_locked(
                &mut peer_store,
                &fixture.b,
                &fixture.policy,
                MusigSessionId([80; 32]),
                None,
            )
            .expect("peer hot session");
        }

        let mut tx = transaction(&fixture.policy, 0);
        crate::keys::musig_sessions::start_attempt(&mut tx).expect("attempt id");
        let bytes = bwk::bwk_tx::bwk_psbt::PsbtV2::from_bitcoin_psbt(tx.clone())
            .expect("v2")
            .serialize()
            .expect("serialize");
        let mut routes = std::collections::BTreeMap::new();
        routes.insert(fixture.fingerprint, RouteKind::Qr);
        routes.insert(fixture.b.fingerprint(), RouteKind::Hot);
        let (session_id, request) = crate::keys::musig_sessions::allocate_first_request_at(
            fixture.root(),
            &fixture.policy,
            &bytes,
            &routes,
            fixture.fingerprint,
        )
        .expect("first request allocation");
        assert_eq!(session_id, preparation.session_id);
        let allocated = bwk::bwk_tx::bwk_psbt::PsbtV2::deserialize(&request)
            .expect("v2")
            .into_bitcoin_psbt()
            .expect("v0");
        let parsed = fixture.parsed();
        for signer in [fixture.fingerprint, fixture.b.fingerprint()] {
            let slots =
                bwk::bwk_sign::musig::own_slots(&allocated, &parsed, signer).expect("own slots");
            assert!(
                !slots.is_empty()
                    && slots.iter().all(|slot| {
                        bwk::bwk_tx::bwk_psbt::musig::musig_input_pub_nonce(
                            &allocated.inputs[slot.input_index],
                            slot.participant,
                            slot.aggregate,
                            slot.leaf_hash,
                        )
                        .expect("nonce field")
                        .is_some()
                    }),
                "every required signer's nonces ride on the request"
            );
        }
        // The peer's hot session was allocated too, and the QR transcript is pinned.
        let peer_store =
            LockedCache::open(fixture.root(), fixture.b.fingerprint()).expect("peer cache");
        assert!(
            peer_store.state.available.is_empty(),
            "the hot session was taken for this attempt"
        );
        let store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("own cache");
        assert!(
            store.state.allocated[0].qr_transcript.is_some(),
            "the transcript is pinned in the same pass"
        );
    }

    #[test]
    fn allocate_first_request_fails_without_touching_any_cache_when_a_signer_is_not_ready() {
        let fixture = fixture();
        let session = device_session(&fixture.a, &fixture, [11; 32]);
        let preparation = prepare(&fixture);
        accept_prepared_ok(&fixture, &session, preparation.session_id);
        // The peer has no hot session: the whole pass must refuse.
        let mut tx = transaction(&fixture.policy, 0);
        crate::keys::musig_sessions::start_attempt(&mut tx).expect("attempt id");
        let bytes = bwk::bwk_tx::bwk_psbt::PsbtV2::from_bitcoin_psbt(tx)
            .expect("v2")
            .serialize()
            .expect("serialize");
        let mut routes = std::collections::BTreeMap::new();
        routes.insert(fixture.fingerprint, RouteKind::Qr);
        routes.insert(fixture.b.fingerprint(), RouteKind::Hot);
        assert!(
            crate::keys::musig_sessions::allocate_first_request_at(
                fixture.root(),
                &fixture.policy,
                &bytes,
                &routes,
                fixture.fingerprint,
            )
            .is_err(),
            "a required signer without a cached session fails the request"
        );
        let store = LockedCache::open(fixture.root(), fixture.fingerprint).expect("own cache");
        assert!(
            store.state.allocated.is_empty(),
            "the QR session was not taken by a failed request"
        );
        assert_eq!(
            store.state.available.len(),
            1,
            "the preparation stayed available"
        );
    }
}
