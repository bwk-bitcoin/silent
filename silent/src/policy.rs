//! CXX surface over bwk-bup: compile a creator policy, lift a descriptor for review.

use std::str::FromStr;

use bitcoin::{
    absolute,
    bip32::{ChildNumber, DerivationPath, Fingerprint, Xpriv, Xpub},
    hashes::{sha256, Hash, HashEngine},
    relative,
    secp256k1::Secp256k1,
    NetworkKind,
};
use bwk::bwk_descriptor::{descriptor::Descriptor, musig::musig_descriptor_to_string};
use bwk_bup::{
    infer_policy_type, policy::MAX_LEAVES, Bip87Account, KeyType, Locktime, OXpub, Path, Policy,
    PolicyError, PolicyType, Semantic, TapPosition,
};

use crate::ffi::{
    Network, PolicyCompileResult, PolicyKey, PolicyKeyType, PolicyLocktime, PolicyPath,
    PolicyReview, PolicyReviewKey, PolicyReviewPath, PolicySemantic, PolicySpec,
};

/// Empty when `account` does not fit a hardened index, where bwk-bup would panic.
pub fn bip87_account_path(network: Network, account: u32) -> String {
    if ChildNumber::from_hardened_idx(account).is_err() {
        return String::new();
    }
    let network = NetworkKind::from(bitcoin::Network::from(network));
    format!(
        "m/{}",
        bwk_bup::key_role::bip87_account_path(network, account)
    )
}

pub fn preview_policy(spec: PolicySpec) -> PolicyCompileResult {
    compile_spec(&spec, true)
}

pub fn compile_policy(spec: PolicySpec) -> PolicyCompileResult {
    compile_spec(&spec, false)
}

pub fn review_policy(descriptor: String) -> PolicyReview {
    match lift(&descriptor) {
        Ok(policy) => to_review(&policy),
        Err(error) => PolicyReview {
            is_ok: false,
            error,
            paths: Vec::new(),
            leaf_count: 0,
        },
    }
}

fn compile_spec(spec: &PolicySpec, placeholder_keys: bool) -> PolicyCompileResult {
    let (paths, policy_type) = match to_paths(spec, placeholder_keys) {
        Ok(paths) => paths,
        Err((error_path, error)) => return compile_error(error_path, error, false, 0),
    };
    let mut policy = match Policy::new(paths, policy_type) {
        Ok(policy) => policy,
        Err(e) => return policy_error(e),
    };
    let descriptor = match policy.compile() {
        Ok(_) if placeholder_keys => String::new(),
        Ok(descriptor) => musig_descriptor_to_string(descriptor, true),
        Err(e) => return policy_error(e),
    };
    PolicyCompileResult {
        is_ok: true,
        error: String::new(),
        error_path: -1,
        too_many_leaves: false,
        descriptor,
        leaf_count: policy.leaf_count() as u32,
        max_leaves: MAX_LEAVES as u32,
        has_key_path: policy
            .paths()
            .iter()
            .any(|path| matches!(path.position(), TapPosition::InternalKey)),
    }
}

/// `leaf_count` is the count the policy needs on TooManyLeaves, 0 otherwise.
fn compile_error(
    error_path: i32,
    error: String,
    too_many_leaves: bool,
    leaf_count: u32,
) -> PolicyCompileResult {
    PolicyCompileResult {
        is_ok: false,
        error,
        error_path,
        too_many_leaves,
        descriptor: String::new(),
        leaf_count,
        max_leaves: MAX_LEAVES as u32,
        has_key_path: false,
    }
}

fn policy_error(e: PolicyError) -> PolicyCompileResult {
    match e {
        PolicyError::TooManyLeaves { count, .. } => {
            compile_error(-1, e.to_string(), true, count as u32)
        }
        e => compile_error(-1, e.to_string(), false, 0),
    }
}

fn to_paths(
    spec: &PolicySpec,
    placeholder_keys: bool,
) -> Result<(Vec<Path>, PolicyType), (i32, String)> {
    let network = bitcoin::Network::from(spec.network);
    let paths = spec
        .paths
        .iter()
        .enumerate()
        .map(|(i, path)| to_path(path, network, placeholder_keys).map_err(|e| (i as i32, e)))
        .collect::<Result<Vec<_>, _>>()?;
    let policy_type = infer_policy_type(&paths);
    Ok((paths, policy_type))
}

fn to_path(
    path: &PolicyPath,
    network: bitcoin::Network,
    placeholder_keys: bool,
) -> Result<Path, String> {
    let keys = path
        .keys
        .iter()
        .map(|key| role_key(key, network, placeholder_keys))
        .collect::<Result<Vec<_>, _>>()?;
    let semantic = to_semantic(path, keys)?;
    let locktime = to_locktime(path.locktime, path.locktime_value)?;
    semantic
        .validate()
        .map_err(|e| PolicyError::from(e).to_string())?;
    locktime
        .validate()
        .map_err(|e| PolicyError::from(e).to_string())?;
    Ok(Path::new(semantic, locktime, TapPosition::TapTree(vec![])))
}

/// `[fp/87'/coin'/account'/R]xpub` of `key`.
fn role_key(
    key: &PolicyKey,
    network: bitcoin::Network,
    placeholder_keys: bool,
) -> Result<OXpub, String> {
    let fingerprint = Fingerprint::from_str(&key.fingerprint)
        .map_err(|e| format!("invalid fingerprint {}: {e}", key.fingerprint))?;
    let path = DerivationPath::from_str(&key.account_path)
        .map_err(|e| format!("invalid account path {}: {e}", key.account_path))?;
    let key_type =
        to_key_type(key.key_type).ok_or_else(|| format!("key {fingerprint} has no role"))?;
    let xpub = if placeholder_keys {
        placeholder_xpub(fingerprint, &path, network)?
    } else {
        Xpub::from_str(&key.account_xpub)
            .map_err(|e| format!("invalid xpub for key {fingerprint}: {e}"))?
    };
    Bip87Account::new(fingerprint, path, xpub)
        .and_then(|account| account.role_key(key_type, 0))
        .map_err(|e| format!("key {fingerprint}: {e}"))
}

/// A stand-in account xpub, the same for the same fingerprint and account path, so a
/// preview counts leaves and checks key reuse as the real compile does.
fn placeholder_xpub(
    fingerprint: Fingerprint,
    path: &DerivationPath,
    network: bitcoin::Network,
) -> Result<Xpub, String> {
    let mut engine = sha256::Hash::engine();
    engine.input(fingerprint.as_bytes());
    engine.input(path.to_string().as_bytes());
    let seed = sha256::Hash::from_engine(engine);
    let master = Xpriv::new_master(network, seed.as_byte_array())
        .map_err(|e| format!("placeholder key for {fingerprint}: {e}"))?;
    Ok(Xpub::from_priv(&Secp256k1::new(), &master))
}

fn to_semantic(path: &PolicyPath, keys: Vec<OXpub>) -> Result<Semantic, String> {
    let threshold = path.threshold as usize;
    match path.semantic {
        PolicySemantic::Single => single(keys, threshold),
        PolicySemantic::Multi => Ok(Semantic::Multi { keys, threshold }),
        PolicySemantic::MultiMandatory => Ok(multi_mandatory(path, keys, threshold)),
        PolicySemantic::Other => Err("unsupported path shape".into()),
        // SAFETY: PolicySemantic is defined in the CXX bridge with exactly 4 variants.
        _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
    }
}

fn single(keys: Vec<OXpub>, threshold: usize) -> Result<Semantic, String> {
    let key_count = keys.len();
    match (keys.into_iter().next(), key_count, threshold) {
        (Some(key), 1, 1) => Ok(Semantic::Single(key)),
        _ => Err(format!(
            "a single key path needs one key and threshold 1, got {key_count} keys and threshold {threshold}"
        )),
    }
}

/// `keys` follows `path.keys`, whose `mandatory` flags split it.
fn multi_mandatory(path: &PolicyPath, keys: Vec<OXpub>, threshold: usize) -> Semantic {
    let (mandatory, cosigners): (Vec<_>, Vec<_>) = path
        .keys
        .iter()
        .zip(keys)
        .partition(|(key, _)| key.mandatory);
    Semantic::MultiMandatory {
        keys: cosigners.into_iter().map(|(_, key)| key).collect(),
        mandatory_keys: mandatory.into_iter().map(|(_, key)| key).collect(),
        threshold,
    }
}

fn to_locktime(kind: PolicyLocktime, value: u32) -> Result<Locktime, String> {
    match kind {
        PolicyLocktime::None => Ok(Locktime::None),
        PolicyLocktime::Relative => u16::try_from(value)
            .map(|blocks| Locktime::Relative(relative::LockTime::from_height(blocks)))
            .map_err(|_| format!("relative timelock {value} is above {} blocks", u16::MAX)),
        PolicyLocktime::AbsoluteRenewable => {
            absolute_height(value).map(Locktime::AbsoluteRenewable)
        }
        PolicyLocktime::Absolute => absolute_height(value).map(Locktime::Absolute),
        // SAFETY: PolicyLocktime is defined in the CXX bridge with exactly 4 variants.
        _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
    }
}

fn absolute_height(value: u32) -> Result<absolute::LockTime, String> {
    absolute::LockTime::from_height(value).map_err(|e| e.to_string())
}

fn to_key_type(key_type: PolicyKeyType) -> Option<KeyType> {
    match key_type {
        PolicyKeyType::Internal => Some(KeyType::Internal),
        PolicyKeyType::External => Some(KeyType::External),
        PolicyKeyType::Cosigner => Some(KeyType::Cosigner),
        PolicyKeyType::SafetyNet => Some(KeyType::SafetyNet),
        PolicyKeyType::Unknown => None,
        // SAFETY: PolicyKeyType is defined in the CXX bridge with exactly 5 variants.
        _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
    }
}

fn from_key_type(key_type: Option<KeyType>) -> PolicyKeyType {
    match key_type {
        Some(KeyType::Internal) => PolicyKeyType::Internal,
        Some(KeyType::External) => PolicyKeyType::External,
        Some(KeyType::Cosigner) => PolicyKeyType::Cosigner,
        Some(KeyType::SafetyNet) => PolicyKeyType::SafetyNet,
        None => PolicyKeyType::Unknown,
    }
}

fn lift(text: &str) -> Result<Policy, String> {
    let descriptor = Descriptor::from_str(text)
        .and_then(Descriptor::into_miniscript)
        .map_err(|e| e.to_string())?;
    Policy::from_descriptor(&descriptor).map_err(|e| e.to_string())
}

fn to_review(policy: &Policy) -> PolicyReview {
    PolicyReview {
        is_ok: true,
        error: String::new(),
        paths: policy.paths().iter().map(review_path).collect(),
        leaf_count: policy.leaf_count() as u32,
    }
}

fn review_path(path: &Path) -> PolicyReviewPath {
    let (semantic, threshold) = review_semantic(path.semantic());
    let (locktime, locktime_value) = review_locktime(path.locktime());
    let (key_path, leaf_count) = match path.position() {
        TapPosition::InternalKey => (true, 0),
        TapPosition::TapTree(_) => (false, path.leaf_count() as u32),
    };
    PolicyReviewPath {
        semantic,
        threshold,
        locktime,
        locktime_value,
        keys: path
            .key_types()
            .into_iter()
            .map(|(key, key_type)| review_key(path.semantic(), key, key_type))
            .collect(),
        leaf_count,
        key_path,
    }
}

fn review_semantic(semantic: &Semantic) -> (PolicySemantic, u32) {
    match semantic {
        Semantic::Single(_) => (PolicySemantic::Single, 1),
        Semantic::Multi { threshold, .. } => (PolicySemantic::Multi, *threshold as u32),
        Semantic::MultiMandatory { threshold, .. } => {
            (PolicySemantic::MultiMandatory, *threshold as u32)
        }
        _ => (PolicySemantic::Other, 0),
    }
}

fn review_locktime(locktime: &Locktime) -> (PolicyLocktime, u32) {
    match locktime {
        Locktime::None => (PolicyLocktime::None, 0),
        Locktime::Relative(lock) => (PolicyLocktime::Relative, lock.to_consensus_u32()),
        Locktime::AbsoluteRenewable(lock) => {
            (PolicyLocktime::AbsoluteRenewable, lock.to_consensus_u32())
        }
        Locktime::Absolute(lock) => (PolicyLocktime::Absolute, lock.to_consensus_u32()),
    }
}

fn review_key(semantic: &Semantic, key: OXpub, key_type: Option<KeyType>) -> PolicyReviewKey {
    let mandatory = matches!(
        semantic,
        Semantic::MultiMandatory { mandatory_keys, .. } if mandatory_keys.contains(&key)
    );
    let (fingerprint, origin_path) = match &key.origin {
        Some((fingerprint, path)) => (fingerprint.to_string(), format!("m/{path}")),
        None => (String::new(), String::new()),
    };
    PolicyReviewKey {
        fingerprint,
        origin_path,
        xpub: key.xkey.to_string(),
        key_type: from_key_type(key_type),
        mandatory,
    }
}
