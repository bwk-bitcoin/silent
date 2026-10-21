use crate::ffi::{BackendErrorKind, Notification, NotificationFlag, PsbtValidation, TxResult};

#[derive(Debug, thiserror::Error)]
#[error("{message}")]
pub(crate) struct BackendFailure {
    pub kind: BackendErrorKind,
    pub message: String,
}

impl BackendFailure {
    pub(crate) fn new(kind: BackendErrorKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
        }
    }
}

impl TxResult {
    pub(crate) fn failed(kind: BackendErrorKind, error: String) -> Self {
        Self {
            is_ok: false,
            kind,
            error,
            value: String::new(),
        }
    }
    pub(crate) fn succeeded(value: String) -> Self {
        Self {
            is_ok: true,
            kind: BackendErrorKind::None,
            error: String::new(),
            value,
        }
    }
}

pub fn validation_error_kind(result: &PsbtValidation) -> BackendErrorKind {
    match (
        result.spent_input_count,
        result.reused_output_count,
        result.is_ok,
    ) {
        (1.., _, _) => BackendErrorKind::InputConflict,
        (_, 1.., _) => BackendErrorKind::AddressReuse,
        (_, _, false) => BackendErrorKind::Electrum,
        _ => BackendErrorKind::None,
    }
}
pub fn notification_error_kind(notification: &Notification) -> BackendErrorKind {
    match notification.flag {
        NotificationFlag::FailStartScanning | NotificationFlag::FailScan => {
            BackendErrorKind::Backend
        }
        NotificationFlag::ElectrumError => BackendErrorKind::Electrum,
        _ => BackendErrorKind::Other,
    }
}
