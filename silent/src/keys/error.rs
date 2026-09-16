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
}
