use std::path::PathBuf;

use bitcoin::bip32::Fingerprint;

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
}
