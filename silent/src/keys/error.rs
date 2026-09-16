use std::path::PathBuf;

use bitcoin::bip32::{DerivationPath, Fingerprint, Xpub};

use crate::keys::record::RouteKind;

/// Errors from the `keys` module: the record model, and everything later built on it
/// (storage, crypto, route execution).
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("unsupported network '{0}'")]
    UnsupportedNetwork(String),
    #[error("route '{0}' is already present")]
    DuplicateRoute(RouteKind),
    #[error("the hot route must be first")]
    HotRouteNotFirst,
    #[error("route '{0}' was not found")]
    RouteNotFound(RouteKind),
    #[error("the new route order is not a permutation of the current routes")]
    RouteOrderMismatch,
    #[error("key record version {found} is not supported, this build supports up to {supported}")]
    UnsupportedRecordVersion { found: u32, supported: u32 },
    #[error("io error on {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("failed to parse {path}: {source}")]
    Parse {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
    #[error("key '{0}' was not found")]
    NotFound(Fingerprint),
    #[error("file {path} has fingerprint '{expected}' but record has '{found}'")]
    FingerprintMismatch {
        path: PathBuf,
        expected: Fingerprint,
        found: Fingerprint,
    },
    #[error("expected network '{expected}' but record has network '{found}'")]
    NetworkMismatch { expected: String, found: String },
    #[error("file {path} has an invalid fingerprint file name '{stem}'")]
    InvalidFingerprintFileName { path: PathBuf, stem: String },
    #[error("wrong password")]
    WrongPassword,
    #[error("crypto error: {0}")]
    Crypto(String),
    #[error("invalid base64 in secret envelope: {0}")]
    Base64(String),
    #[error("failed to generate random bytes: {0}")]
    Random(String),
    #[error("kdf parameter '{field}' is {found}, below the minimum of {minimum}")]
    WeakKdfParams {
        field: &'static str,
        found: u32,
        minimum: u32,
    },
    #[error("unsupported kdf '{0}'")]
    UnsupportedKdf(String),
    #[error("unsupported cipher '{0}'")]
    UnsupportedCipher(String),
    #[error(
        "secret envelope version {found} is not supported, this build supports up to {supported}"
    )]
    UnsupportedEnvelopeVersion { found: u32, supported: u32 },
    #[error("key '{0}' is already registered")]
    DuplicateKey(Fingerprint),
    #[error("key '{0}' is not registered")]
    UnknownKey(Fingerprint),
    #[error("key '{fingerprint}' has {count} descriptor(s) registered")]
    DescriptorsRegistered {
        fingerprint: Fingerprint,
        count: usize,
    },
    #[error("path '{path}' on key '{fingerprint}' is already cached with a different xpub")]
    XpubMismatch {
        fingerprint: Fingerprint,
        path: DerivationPath,
        cached: Box<Xpub>,
        found: Box<Xpub>,
    },
    #[error("key '{fingerprint}' has no descriptor '{descriptor}' registered")]
    DescriptorNotRegistered {
        fingerprint: Fingerprint,
        descriptor: String,
    },
    #[error("failed to parse PSBT: {0}")]
    PsbtParse(String),
    #[error("failed to serialize PSBT: {0}")]
    PsbtSerialize(String),
    #[error("malformed silent payment PSBT field: {0}")]
    SpField(String),
    #[error("PSBT carries silent payment artifacts but no sp() descriptor was supplied")]
    MissingSpDescriptor,
    #[error("sp() descriptor '{0}' has no key origin")]
    MissingSpOrigin(String),
    #[error("unsupported mnemonic word count {0}")]
    InvalidWordCount(usize),
    #[error("invalid mnemonic: {0}")]
    Mnemonic(String),
    #[error("key '{0}' has no secret to reveal")]
    NoSecret(Fingerprint),
    #[error("key '{0}' already has a secret")]
    SecretAlreadyPresent(Fingerprint),
    #[error("hot signer error: {0}")]
    Signer(String),
    #[error("silent payment signer error: {0}")]
    SpSign(String),
    #[error("failed to parse descriptor: {0}")]
    DescriptorParse(String),
    #[error("sp() descriptor holds a spend secret; only watch-only descriptors are accepted")]
    SpSpendDescriptor,
    #[error(
        "the psbt still has an output with no script: the sp() key must sign first to \
         derive it"
    )]
    SpOutputsPending,
    #[error("no input was signed with this key")]
    NothingSigned,
}
