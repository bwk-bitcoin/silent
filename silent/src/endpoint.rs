use std::collections::VecDeque;

use crate::{
    account::Account,
    ffi::{BackendErrorKind, BackendServer, EndpointPhase, EndpointResult, RetryDecision},
    musig::MusigAccount,
    tx_details::test_and_save_endpoint,
};

const LOSS_WINDOW_MS: i64 = 60_000;
const RETRY_DELAY_MS: u32 = 2000;
const MAX_LOSSES: usize = 3;

#[derive(Default)]
pub struct EndpointRetry {
    armed: bool,
    up: bool,
    retrying: bool,
    losses: VecDeque<i64>,
}

pub fn new_endpoint_retry() -> Box<EndpointRetry> {
    Box::default()
}

impl EndpointRetry {
    pub fn retry_connected(&mut self) {
        self.armed = true;
        self.up = true;
        self.retrying = false;
    }
    pub fn retry_stopped(&mut self) {
        self.armed = false;
        self.up = false;
        self.retrying = false;
    }
    pub fn retry_lost(&mut self, now_ms: i64) -> RetryDecision {
        if !self.armed || (!self.up && !self.retrying) {
            return RetryDecision::default();
        }
        self.up = false;
        self.retrying = false;
        self.losses
            .retain(|at| now_ms.saturating_sub(*at) <= LOSS_WINDOW_MS);
        self.losses.push_back(now_ms);
        let losses = self.losses.len() as u32;
        if self.losses.len() > MAX_LOSSES {
            self.losses.clear();
            self.retry_stopped();
            RetryDecision {
                give_up: true,
                losses,
                ..Default::default()
            }
        } else {
            RetryDecision {
                schedule: true,
                delay_ms: RETRY_DELAY_MS,
                losses,
                give_up: false,
            }
        }
    }
    pub fn retry_due(&mut self) -> bool {
        if !self.armed {
            return false;
        }
        self.retrying = true;
        true
    }
}

struct Change {
    server: BackendServer,
    url: String,
    verify: bool,
    test: bool,
    connect: bool,
    resume: bool,
    phase: EndpointPhase,
    result: EndpointResult,
}

impl Change {
    fn new(
        server: BackendServer,
        url: String,
        verify: bool,
        test: bool,
        connect: bool,
        resume: bool,
    ) -> Self {
        Self {
            server,
            url,
            verify,
            test,
            connect,
            resume,
            phase: EndpointPhase::Ready,
            result: EndpointResult {
                kind: BackendErrorKind::Other,
                error: "Endpoint change has not completed".into(),
                ..Default::default()
            },
        }
    }
    fn ready(&mut self) -> bool {
        if self.phase != EndpointPhase::Ready {
            self.result.is_ok = false;
            self.result.kind = BackendErrorKind::InvalidInput;
            self.result.error = "Endpoint change already started".into();
            return false;
        }
        true
    }
    fn test_settings(&mut self, name: String) -> bool {
        if self.test {
            self.result = test_and_save_endpoint(name, self.server, self.url.clone(), self.verify);
            if !self.result.is_ok {
                self.phase = EndpointPhase::Complete;
                return false;
            }
        }
        self.phase = EndpointPhase::Draining;
        self.result.is_ok = false;
        self.result.kind = BackendErrorKind::Other;
        self.result.error = "Notification receiver drain required before reopening".into();
        true
    }
    fn reopened(&mut self, mut result: EndpointResult) {
        result.saved = self.result.saved;
        if result.saved {
            result.url = self.result.url.clone();
        }
        self.result = result;
        self.phase = EndpointPhase::Complete;
    }
    fn reject_finish(&mut self) {
        self.result.is_ok = false;
        self.result.kind = BackendErrorKind::InvalidInput;
        self.result.error = "Endpoint change is not awaiting receiver drain".into();
    }
    fn unavailable(&mut self, error: String) {
        self.phase = EndpointPhase::Complete;
        self.result = EndpointResult {
            kind: BackendErrorKind::Other,
            error,
            ..Default::default()
        };
    }
}

// Each command owns the account while Qt drains the workers and notification receiver.
pub struct SpEndpointChange {
    account: Box<Account>,
    change: Change,
}
pub struct MusigEndpointChange {
    account: Box<MusigAccount>,
    change: Change,
}

pub fn sp_startup_endpoint_change(account: Box<Account>) -> Box<SpEndpointChange> {
    let connect = account.sub_account_count() > 0 && !account.electrum_url().is_empty();
    let server = if connect {
        BackendServer::Electrum
    } else {
        BackendServer::Blindbit
    };
    Box::new(SpEndpointChange {
        account,
        change: Change::new(server, String::new(), true, false, connect, true),
    })
}

pub fn sp_endpoint_change(
    account: Box<Account>,
    server: BackendServer,
    url: String,
    verify: bool,
    test: bool,
    connected: bool,
) -> Box<SpEndpointChange> {
    let connect = if server == BackendServer::Electrum {
        connected
    } else {
        account.electrum_running()
    };
    let resume = if server == BackendServer::Blindbit {
        connected
    } else {
        account.scanner_running()
    };
    Box::new(SpEndpointChange {
        account,
        change: Change::new(server, url, verify, test, connect, resume),
    })
}

pub fn musig_endpoint_change(
    account: Box<MusigAccount>,
    url: String,
    verify: bool,
    test: bool,
    connected: bool,
) -> Box<MusigEndpointChange> {
    Box::new(MusigEndpointChange {
        account,
        change: Change::new(BackendServer::Electrum, url, verify, test, connected, false),
    })
}

impl SpEndpointChange {
    pub fn endpoint_server(&self) -> BackendServer {
        self.change.server
    }
    pub fn endpoint_phase(&self) -> EndpointPhase {
        self.change.phase
    }
    pub fn endpoint_result(&self) -> EndpointResult {
        self.change.result.clone()
    }
    pub fn prepare_endpoint(&mut self) {
        if !self.change.ready() {
            return;
        }
        let Some(name) = self.account.endpoint_account_name() else {
            self.change.unavailable(self.account.get_error());
            return;
        };
        if self.change.test_settings(name.to_owned()) {
            self.account.close_backend();
        }
    }
    pub fn finish_endpoint_after_drain(&mut self) {
        if self.change.phase != EndpointPhase::Draining {
            self.change.reject_finish();
            return;
        }
        let mut result = self.account.reopen_backend(self.change.connect);
        result.connection_pending &= self.account.sub_account_count() > 0;
        // Do not replace an earlier reopen error with the scanner error.
        if self.account.is_ok()
            && self.change.resume
            && !self.account.start_scanner()
            && result.is_ok
        {
            self.change.server = BackendServer::Blindbit;
            result.is_ok = false;
            result.kind = BackendErrorKind::Backend;
            result.error = "Backend reopened but scanner could not start".into();
        }
        self.change.reopened(result);
    }
}

impl MusigEndpointChange {
    pub fn endpoint_server(&self) -> BackendServer {
        BackendServer::Electrum
    }
    pub fn endpoint_phase(&self) -> EndpointPhase {
        self.change.phase
    }
    pub fn endpoint_result(&self) -> EndpointResult {
        self.change.result.clone()
    }
    pub fn prepare_endpoint(&mut self) {
        if !self.change.ready() {
            return;
        }
        let Some(name) = self.account.endpoint_account_name() else {
            self.change.unavailable(self.account.get_error());
            return;
        };
        if self.change.test_settings(name.to_owned()) {
            self.account.close_backend();
        }
    }
    pub fn finish_endpoint_after_drain(&mut self) {
        if self.change.phase != EndpointPhase::Draining {
            self.change.reject_finish();
            return;
        }
        let result = self.account.reopen_backend(self.change.connect);
        self.change.reopened(result);
    }
}

// CXX consumes the opaque command to transfer its account back exactly once.
#[allow(clippy::boxed_local)]
pub fn take_sp_endpoint_account(change: Box<SpEndpointChange>) -> Box<Account> {
    change.account
}
#[allow(clippy::boxed_local)]
pub fn take_musig_endpoint_account(change: Box<MusigEndpointChange>) -> Box<MusigAccount> {
    change.account
}
