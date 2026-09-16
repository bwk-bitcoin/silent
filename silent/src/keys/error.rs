use std::path::PathBuf;

use bitcoin::{
    bip32::{DerivationPath, Fingerprint, Xpub},
    Txid,
};

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
    #[error("the psbt's sp inputs disagree on which key must sign")]
    SpInputsDisagree,
    #[error("no registered sp() key matches{}", fmt_sp_key_unknown(.0))]
    SpKeyUnknown(Option<Fingerprint>),
    #[error("the psbt has a pending sp output but no sp input data to resolve its signer")]
    SpOutputsUnresolvable,
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
    #[error("signed psbt is for a different transaction: expected {expected}, got {got}")]
    SignedPsbtMismatch { expected: Txid, got: Txid },
    #[error("no input was signed with this key")]
    NothingSigned,
    #[error("QR frame index {index} is out of range: only {count} frame(s) available")]
    FrameOutOfRange { index: usize, count: usize },
    #[error("QR encoder produced no frames")]
    NoFrames,
    #[error("QR request id {got:?} is stale: {expected:?} was reserved after it")]
    StaleRequest {
        expected: bwk_qr_protocol::RequestId,
        got: bwk_qr_protocol::RequestId,
    },
    #[error("get_xpubs requires at least one derivation path")]
    EmptyPathList,
    #[error("QR payload is a request, not a response")]
    PayloadIsRequest,
    #[error("no QR request is outstanding")]
    NoOutstandingRequest,
    #[error("response id {got} does not match outstanding request id {expected}")]
    RequestIdMismatch { expected: String, got: String },
    #[error("expected a response to {expected:?}, got {got:?}")]
    ResponseKindMismatch {
        expected: bwk_qr_protocol::MessageType,
        got: bwk_qr_protocol::MessageType,
    },
    #[error("signer refused {message_type:?} with {code:?}: {message}")]
    SignerRefused {
        message_type: bwk_qr_protocol::MessageType,
        code: bwk_qr_protocol::response::Error,
        message: String,
    },
    #[error("get_xpubs requested {requested} path(s) but the response carries {returned}")]
    XpubCountMismatch { requested: usize, returned: usize },
    #[error("signed PSBT failed to parse as PSBT v2 or v0")]
    InvalidSignedPsbt,
    #[error("signer returned bare signatures, only a full signed PSBT is accepted")]
    SignaturesUnsupported,
    #[error("sp() descriptor has no key origin")]
    SpDescriptorNoOrigin,
    #[error("registration response did not report whether registration succeeded")]
    IncompleteRegistration,
    #[error("address verification response carries no address uri")]
    NoAddressUri,
    #[error("file {path} already exists")]
    FileExists { path: PathBuf },
    #[error("psbt file {path} is {len} bytes, over the accepted size limit")]
    PsbtFileTooLarge { path: PathBuf, len: u64 },
    #[error("could not read a psbt from {path}: tried {tried}")]
    UnreadablePsbtFile { path: PathBuf, tried: &'static str },
    #[error("camera capture is only available on Linux; paste the QR response instead")]
    CameraUnsupported,
    #[error("failed to open camera {index}: {message}")]
    CameraOpen { index: u32, message: String },
    #[error("camera capture error: {0}")]
    CameraCapture(String),
    #[error("no camera was found")]
    NoCamera,
    #[error("no camera format meets the minimum resolution and frame rate; offered: {0}")]
    CameraNoSuitableFormat(String),
    #[error("camera {0} is already in use")]
    CameraBusy(u32),
    #[error(transparent)]
    Qr(#[from] bwk_qr::error::Error),
    #[error(transparent)]
    Encode(#[from] bwk_qr_protocol::encode::Error),
    #[error(transparent)]
    Decode(#[from] bwk_qr_protocol::decode::Error),
    #[error(transparent)]
    XpubConvert(#[from] bwk_qr_protocol::types::Error),
}

fn fmt_sp_key_unknown(fingerprint: &Option<Fingerprint>) -> String {
    match fingerprint {
        Some(fingerprint) => format!(" fingerprint '{fingerprint}'"),
        None => " the psbt's input tweaks".to_string(),
    }
}
