//! MusigAccount: CXX surface over bwk::Account and saved transaction progress.

use std::{
    collections::BTreeSet,
    str::FromStr,
    sync::{mpsc, Arc, Mutex, MutexGuard},
};

use bitcoin::consensus::encode::{deserialize_hex, serialize_hex};
use bitcoin::taproot::TapLeafHash;
use bwk::account::{Account as BwkAccount, SpendLeaf, SpendPath};
use bwk::bwk_coin::{CoinStatus, KeyChain};
use bwk::bwk_descriptor::{
    descriptor::{Descriptor, DescriptorDerivator},
    wallet::WalletDescriptorExt,
};
use bwk::bwk_electrum::notification::Notification as BwkNotification;
use bwk::bwk_sign::musig::MusigRound as BwkMusigRound;
use bwk::bwk_tx::{
    bwk_psbt::PsbtV2, recipient::Recipient, transaction::Amount, tx_builder::TxBuilder,
};
use bwk_sp::receiver::RecipientAddress;

use crate::{
    account::{
        broadcast_via_electrum, convert_notification, parse_outpoint, psbt_validation, rust_txs,
        NotificationReceiver, Poll, PsbtResult, TemplateError,
    },
    account_storage::{AccountStorage, Factory},
    config::{parse_electrum_url, Config},
    ffi::{
        AddressVerifyInfo, CoinState, MusigAttemptSnapshot, MusigCosignerStatus,
        MusigDescriptorResult, MusigElectrumUrlResult, MusigFinalizeResult, MusigReceiveAddress,
        MusigRound, MusigRoundStatus, MusigSpendLeaf, MusigSpendLeaves, Network, PsbtValidation,
        RustCoin, RustTx, RustTxDetails, SavedPsbtEntry, SavedPsbtList, SavedPsbtMutation,
        SavedPsbtReset, SavedPsbtRestore, TransactionSimulation, TransactionTemplate, TxResult,
    },
    keys::{
        bridge::{merge_signed_psbt, musig_session_restart},
        psbt::ordinary_signature_complete,
    },
    saved_psbt::{apply_chain_state, frozen_entry, mutation_error, SavedPsbtStore},
    tx_details::{details_result, TxSnapshot},
};

/// A MuSig2 policy account, wrapping bwk::Account.
/// Always constructible; check is_ok() before use.
pub struct MusigAccount {
    inner: Option<MusigAccountInner>,
    error: String,
    closed_saved_psbts: Option<Mutex<SavedPsbtStore>>,
    closed_name: Option<String>,
    pending_flush_error: String,
    active: Mutex<Option<MusigAttemptSnapshot>>,
}

struct MusigAccountInner {
    account: BwkAccount,
    factory: Arc<Factory>,
    receiver: Option<mpsc::Receiver<BwkNotification>>,
    config: Config,
    saved_psbts: Mutex<SavedPsbtStore>,
}

pub const MUSIG_PLUGIN_ID: &str = "musig";

/// The receive index a new account hands out first: bwk moves its tip off 0 before use.
const FIRST_RECEIVE_INDEX: u32 = 1;

const NOT_INITIALIZED: &str = "Account not initialized";

/// bwk's plain tx builder has no silent payment updater to derive such an output.
const SP_RECIPIENT_UNSUPPORTED: &str =
    "silent payment recipients are not supported by this account";

/// Open stored wallet state without starting network services.
pub fn new_musig_account_offline(config: &Config) -> Box<MusigAccount> {
    match MusigAccount::new_inner_connected(config.clone(), false) {
        Ok(inner) => Box::new(MusigAccount {
            inner: Some(inner),
            error: String::new(),
            closed_saved_psbts: None,
            closed_name: None,
            pending_flush_error: String::new(),
            active: Mutex::new(None),
        }),
        Err(error) => {
            log::error!("Failed to open account offline: {error}");
            Box::new(MusigAccount {
                inner: None,
                error,
                closed_saved_psbts: None,
                closed_name: Some(config.account_name.clone()),
                pending_flush_error: String::new(),
                active: Mutex::new(None),
            })
        }
    }
}

pub fn new_musig_account(account_name: String) -> Box<MusigAccount> {
    let config = match Config::from_file(account_name) {
        Ok(c) => c,
        Err(e) => {
            let error = format!("failed to load config: {e}");
            log::error!("{error}");
            return Box::new(MusigAccount {
                inner: None,
                error,
                closed_saved_psbts: None,
                closed_name: None,
                pending_flush_error: String::new(),
                active: Mutex::new(None),
            });
        }
    };
    match MusigAccount::new_inner(config) {
        Ok(inner) => Box::new(MusigAccount {
            inner: Some(inner),
            error: String::new(),
            closed_saved_psbts: None,
            closed_name: None,
            pending_flush_error: String::new(),
            active: Mutex::new(None),
        }),
        Err(e) => {
            let error = format!("Failed to create account: {e}");
            log::error!("{error}");
            Box::new(MusigAccount {
                inner: None,
                error,
                closed_saved_psbts: None,
                closed_name: None,
                pending_flush_error: String::new(),
                active: Mutex::new(None),
            })
        }
    }
}

pub fn validate_musig_descriptor(descriptor: String) -> MusigDescriptorResult {
    let error = |error: String| MusigDescriptorResult {
        is_ok: false,
        error,
        descriptor: String::new(),
    };
    match Descriptor::from_str(&descriptor) {
        Ok(parsed) if parsed.is_sp() => error("not a musig account descriptor".to_string()),
        Ok(parsed) => MusigDescriptorResult {
            is_ok: true,
            error: String::new(),
            descriptor: parsed.to_string(),
        },
        Err(e) => error(format!("invalid musig descriptor: {e}")),
    }
}

pub fn musig_descriptor_fingerprints(descriptor: String) -> Vec<String> {
    let parsed = match Descriptor::from_str(&descriptor) {
        Ok(parsed) => parsed,
        Err(e) => {
            log::error!("musig_descriptor_fingerprints: invalid descriptor: {e}");
            return vec![];
        }
    };
    let Some(miniscript) = parsed.as_miniscript() else {
        log::error!("musig_descriptor_fingerprints: not a musig account descriptor");
        return vec![];
    };
    miniscript
        .plain_keys()
        .iter()
        .filter_map(crate::keys::manager::descriptor_key_fingerprint)
        .collect::<BTreeSet<_>>()
        .into_iter()
        .map(|fingerprint| fingerprint.to_string())
        .collect()
}

pub fn musig_first_receive_address(descriptor: String, network: Network) -> MusigReceiveAddress {
    let error = |error: String| MusigReceiveAddress {
        is_ok: false,
        error,
        address: String::new(),
        descriptor: String::new(),
        path: String::new(),
    };
    let parsed = match Descriptor::from_str(&descriptor) {
        Ok(parsed) => parsed,
        Err(e) => return error(format!("invalid musig descriptor: {e}")),
    };
    let Some(miniscript) = parsed.as_miniscript() else {
        return error("not a musig account descriptor".to_string());
    };
    let derivator = match miniscript.spk_derivator(network.into()) {
        Ok(derivator) => derivator,
        Err(e) => return error(format!("cannot derive addresses: {e}")),
    };
    let info = verify_info(&parsed, KeyChain::Receive, FIRST_RECEIVE_INDEX);
    MusigReceiveAddress {
        is_ok: true,
        error: String::new(),
        address: derivator.receive_at(FIRST_RECEIVE_INDEX).to_string(),
        descriptor: info.descriptor,
        path: info.path,
    }
}

impl MusigAccount {
    fn new_inner(config: Config) -> Result<MusigAccountInner, String> {
        Self::new_inner_connected(config, true)
    }

    fn new_inner_connected(config: Config, connect: bool) -> Result<MusigAccountInner, String> {
        let mut musig_config = config.to_musig_config()?;
        musig_config.scanner.set_stay_offline(!connect);
        let factory = Factory::open(&config)?;
        let config_store = factory.config_store();
        let mut account =
            BwkAccount::try_with_backend_factory(musig_config, factory.clone(), config_store, None)
                .map_err(|e| e.to_string())?;
        let receiver = account.receiver();
        let saved_psbts = Mutex::new(SavedPsbtStore::load_logical(
            AccountStorage::open(&config)?,
            factory.clone(),
        ));
        Ok(MusigAccountInner {
            account,
            factory,
            receiver,
            config,
            saved_psbts,
        })
    }

    pub fn shutdown_backend(&mut self) -> String {
        let Some(mut old) = self.inner.take() else {
            let error = self
                .closed_saved_psbts
                .as_ref()
                .map(|store| store.lock().unwrap_or_else(|e| e.into_inner()).persist())
                .unwrap_or_default();
            return if self.pending_flush_error.is_empty() {
                error
            } else {
                self.pending_flush_error.clone()
            };
        };
        old.account.stop_electrum();
        let mut error = old
            .account
            .try_persist()
            .err()
            .map(|e| e.to_string())
            .unwrap_or_default();
        let saved_error = old
            .saved_psbts
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .persist();
        if error.is_empty() {
            error = saved_error;
        }
        old.factory.begin_shutdown();
        drop(old.account);
        if let Some(receiver) = old.receiver {
            while receiver.recv().is_ok() {}
        }
        old.factory.wait_for_backends();
        if !self.pending_flush_error.is_empty() {
            self.pending_flush_error.clone()
        } else if error.is_empty() {
            old.factory.failure()
        } else {
            error
        }
    }

    /// Header workers can outlive account drop; drain the taken receiver before reopening.
    pub fn close_backend(&mut self) {
        if let Some(mut old) = self.inner.take() {
            self.closed_name = Some(old.config.account_name);
            old.account.stop_electrum();
            if let Err(error) = old.account.try_persist() {
                self.pending_flush_error = error.to_string();
            }
            let saved_error = old
                .saved_psbts
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .persist();
            if self.pending_flush_error.is_empty() {
                self.pending_flush_error = saved_error;
            }
            self.closed_saved_psbts = Some(old.saved_psbts);
            old.factory.begin_shutdown();
            drop(old.account);
            if let Some(receiver) = old.receiver {
                while receiver.recv().is_ok() {}
            }
            old.factory.wait_for_backends();
            if self.pending_flush_error.is_empty() {
                self.pending_flush_error = old.factory.failure();
            }
        }
    }

    pub(crate) fn endpoint_account_name(&self) -> Option<&str> {
        self.inner
            .as_ref()
            .map(|inner| inner.config.account_name.as_str())
            .or(self.closed_name.as_deref())
    }

    /// Called after workers and the old notification receiver have drained.
    pub fn reopen_backend(&mut self, connect_electrum: bool) -> crate::ffi::EndpointResult {
        self.close_backend();
        if !self.pending_flush_error.is_empty() {
            return crate::ffi::EndpointResult {
                kind: crate::ffi::BackendErrorKind::Storage,
                error: self.pending_flush_error.clone(),
                ..Default::default()
            };
        }
        let mut failure_kind = crate::ffi::BackendErrorKind::Other;
        let result = (|| {
            let name = self.closed_name.as_ref().ok_or(NOT_INITIALIZED)?.clone();
            failure_kind = crate::ffi::BackendErrorKind::Storage;
            let config = Config::from_file(name).map_err(|e| e.to_string())?;
            failure_kind = crate::ffi::BackendErrorKind::InvalidInput;
            config.to_musig_config()?;
            failure_kind = crate::ffi::BackendErrorKind::Other;
            let usable = matches!(parse_electrum_url(&config.electrum_url), (Some(_), Some(_)));
            let mut connection_error = None;
            let mut inner =
                match Self::new_inner_connected(config.clone(), connect_electrum && usable) {
                    Ok(inner) => inner,
                    Err(error) if connect_electrum && usable => {
                        connection_error =
                            Some(format!("Cannot connect; backend reopened offline: {error}"));
                        Self::new_inner_connected(config.clone(), false)?
                    }
                    Err(error) => return Err(error),
                };
            // A startup failure has no cached store; use the freshly loaded state.
            inner.saved_psbts = self.closed_saved_psbts.take().unwrap_or(inner.saved_psbts);
            inner
                .saved_psbts
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .bind_factory(inner.factory.clone());
            self.inner = Some(inner);
            self.closed_name = None;
            self.error.clear();
            Ok(crate::ffi::EndpointResult {
                connection_pending: connect_electrum && usable && connection_error.is_none(),
                kind: if connection_error.is_some() || (connect_electrum && !usable) {
                    crate::ffi::BackendErrorKind::Electrum
                } else {
                    crate::ffi::BackendErrorKind::None
                },
                is_ok: (!connect_electrum || usable) && connection_error.is_none(),
                saved: false,
                url: config.electrum_url,
                verify_certificate: config.verify_certificate,
                error: if connect_electrum && !usable {
                    "No usable saved Electrum endpoint".into()
                } else {
                    connection_error.unwrap_or_default()
                },
            })
        })();
        match result {
            Ok(result) => result,
            Err(error) => {
                log::error!("reopen_backend: {error}");
                self.error = error.clone();
                crate::ffi::EndpointResult {
                    kind: failure_kind,
                    error,
                    ..crate::ffi::EndpointResult::default()
                }
            }
        }
    }

    pub fn is_ok(&self) -> bool {
        self.inner.is_some()
    }

    pub fn get_error(&self) -> String {
        if self.pending_flush_error.is_empty() {
            self.error.clone()
        } else {
            self.pending_flush_error.clone()
        }
    }

    pub fn name(&self) -> String {
        match &self.inner {
            Some(inner) => inner.config.account_name.clone(),
            None => String::new(),
        }
    }

    /// The account's network, Regtest when it failed to load: check `is_ok()` first.
    pub fn network(&self) -> Network {
        match &self.inner {
            Some(inner) => inner.config.network,
            None => Network::Regtest,
        }
    }

    pub fn descriptor(&self) -> String {
        match &self.inner {
            Some(inner) => inner.account.descriptor().to_string(),
            None => String::new(),
        }
    }

    pub fn cosigner_fingerprints(&self) -> Vec<String> {
        musig_descriptor_fingerprints(self.descriptor())
    }

    /// Reload the Electrum endpoint from the config file and start the listener.
    /// False, and nothing started, when the url is empty or does not parse.
    pub fn start_electrum(&mut self) -> bool {
        let Some(inner) = &mut self.inner else {
            return false;
        };
        let config = match Config::from_file(inner.config.account_name.clone()) {
            Ok(c) => c,
            Err(e) => {
                log::error!("start_electrum: failed to load config: {e}");
                return false;
            }
        };
        let (Some(host), Some(port)) = parse_electrum_url(&config.electrum_url) else {
            log::error!("start_electrum: no usable electrum url, staying offline");
            inner.config = config;
            return false;
        };
        let endpoint = inner.account.scanner().config().endpoint();
        if endpoint.server() != Some((host.as_str(), port))
            || endpoint.certificate_check()
                != crate::config::certificate_check(config.verify_certificate)
        {
            log::error!("start_electrum: endpoint changed, reopen the backend first");
            return false;
        }
        inner.account.start_electrum();
        true
    }

    pub fn stop_electrum(&mut self) {
        if let Some(inner) = &mut self.inner {
            inner.account.stop_electrum();
        }
    }

    pub fn electrum_url(&self) -> String {
        match &self.inner {
            Some(inner) => inner.config.electrum_url.clone(),
            None => String::new(),
        }
    }

    /// Whether the Electrum server's TLS certificate is checked, true when the account
    /// did not load.
    pub fn verify_certificate(&self) -> bool {
        self.inner
            .as_ref()
            .is_none_or(|inner| inner.config.verify_certificate)
    }

    /// Save only. The controller closes/reopens the backend before connecting.
    pub fn set_electrum_url(&mut self, url: String) -> MusigElectrumUrlResult {
        let Some(inner) = &mut self.inner else {
            return MusigElectrumUrlResult {
                saved: false,
                started: false,
            };
        };
        let mut config = inner.config.clone();
        config.set_electrum_url(url);
        if let Err(e) = config.write_file() {
            log::error!("set_electrum_url: {e}");
            return MusigElectrumUrlResult {
                saved: false,
                started: false,
            };
        }
        inner.config = config;
        self.stop_electrum();
        MusigElectrumUrlResult {
            saved: true,
            started: false,
        }
    }

    /// Try to receive a notification (non-blocking).
    pub fn try_recv(&mut self) -> Box<Poll> {
        let Some(inner) = &mut self.inner else {
            return Box::new(Poll::err(NOT_INITIALIZED));
        };
        let Some(receiver) = &inner.receiver else {
            return Box::new(Poll::err("No notification receiver"));
        };
        match receiver.try_recv() {
            Ok(notif) => Box::new(Poll::ok(convert_notification(notif))),
            Err(mpsc::TryRecvError::Empty) => Box::new(Poll::none()),
            Err(mpsc::TryRecvError::Disconnected) => {
                Box::new(Poll::err("Notification channel disconnected"))
            }
        }
    }

    /// Take ownership of the notification receiver for use in a dedicated thread.
    /// Can only be called once; subsequent calls return a disconnected receiver.
    pub fn take_receiver(&mut self) -> Box<NotificationReceiver> {
        let Some(inner) = self.inner.as_mut() else {
            log::error!("take_receiver: {NOT_INITIALIZED}");
            let (_tx, rx) = mpsc::channel();
            return Box::new(NotificationReceiver::new(rx));
        };
        let Some(receiver) = inner.receiver.take() else {
            log::error!("take_receiver: Notification receiver already taken");
            let (_tx, rx) = mpsc::channel();
            return Box::new(NotificationReceiver::new(rx));
        };
        Box::new(NotificationReceiver::new(receiver))
    }

    pub fn balance(&self) -> u64 {
        match &self.inner {
            Some(inner) => inner.account.scanner().balance().0,
            None => 0,
        }
    }

    pub fn new_addr(&mut self) -> String {
        match &mut self.inner {
            Some(inner) => inner.account.scanner_mut().new_recv_addr().to_string(),
            None => String::new(),
        }
    }

    /// Every coin of the account, spent ones included.
    pub fn coins(&self) -> Vec<RustCoin> {
        let Some(inner) = &self.inner else {
            return vec![];
        };
        let history = inner.account.scanner().tx_history();
        inner
            .account
            .scanner()
            .coins()
            .into_values()
            .map(|entry| RustCoin {
                outpoint: entry.outpoint_str(),
                value: entry.amount_sat(),
                height: entry.height().unwrap_or(0) as u32,
                timestamp: history
                    .iter()
                    .find(|tx| tx.txid() == entry.outpoint().txid)
                    .and_then(|tx| tx.timestamp())
                    .unwrap_or(0),
                label: entry.label(),
                spent: matches!(entry.status(), CoinStatus::Spent | CoinStatus::BeingSpend),
                // A musig account is a taproot wallet.
                account_type: "Taproot".to_string(),
            })
            .collect()
    }

    pub fn spending_coins(&self) -> crate::ffi::SpendCoinsResult {
        let Some(inner) = &self.inner else {
            return Err(NOT_INITIALIZED.to_string()).into();
        };
        let history = inner.account.scanner().tx_history();
        inner
            .account
            .scanner()
            .coins()
            .into_values()
            .map(|entry| {
                let mut coin = crate::spending::descriptor_coin(&entry.coin, inner.config.network)?;
                let coinbase = history
                    .iter()
                    .find(|tx| tx.txid() == entry.coin.outpoint.txid)
                    .map(|tx| tx.tx().is_coinbase());
                crate::spending::set_availability(&mut coin, coinbase);
                Ok(coin)
            })
            .collect::<Result<Vec<_>, _>>()
            .into()
    }

    pub fn spendable_coins(&self) -> CoinState {
        match &self.inner {
            Some(inner) => {
                let state = inner.account.scanner().spendable_coins();
                CoinState {
                    confirmed_count: state.confirmed_coins as u64,
                    confirmed_balance: state.confirmed_balance,
                    unconfirmed_count: state.unconfirmed_coins as u64,
                    unconfirmed_balance: state.unconfirmed_balance,
                }
            }
            None => CoinState {
                confirmed_count: 0,
                confirmed_balance: 0,
                unconfirmed_count: 0,
                unconfirmed_balance: 0,
            },
        }
    }

    pub fn update_coin_label(
        &self,
        outpoint: String,
        label: String,
    ) -> crate::ffi::LabelEditResult {
        self.edit_coin_label(&outpoint, label).into()
    }

    fn edit_coin_label(&self, outpoint: &str, label: String) -> Result<(), String> {
        let inner = self.inner.as_ref().ok_or(NOT_INITIALIZED)?;
        let outpoint = parse_outpoint(outpoint).map_err(|e| e.to_string())?;
        inner
            .account
            .scanner()
            .try_update_coin_label(outpoint, label)
            .map_err(|e| format!("Label is not saved: {e}. Edit it again to retry."))
    }

    /// Payment history, newest first.
    pub fn payment_history(&self) -> Vec<RustTx> {
        match self.tx_snapshot() {
            Ok(snapshot) => snapshot
                .history
                .into_iter()
                .filter(|tx| tx.status != "finalized" && tx.status != "broadcast")
                .collect(),
            Err(error) => {
                log::error!("payment_history: {error}");
                Vec::new()
            }
        }
    }

    pub fn tx_details(&self, txid: &str, allow_network: bool) -> RustTxDetails {
        details_result(self.tx_snapshot().and_then(|snapshot| {
            let inner = self.inner.as_ref().ok_or(NOT_INITIALIZED)?;
            let mut details = snapshot.details(
                txid,
                &inner.config.electrum_url,
                inner.config.verify_certificate,
                allow_network,
            )?;
            crate::tx_details::apply_scanner_labels(&mut details, inner.account.scanner())?;
            Ok(details)
        }))
    }

    fn tx_snapshot(&self) -> Result<TxSnapshot, String> {
        let inner = self.inner.as_ref().ok_or(NOT_INITIALIZED)?;
        let mut snapshot = TxSnapshot::new(
            inner.config.network.into(),
            rust_txs(inner.account.scanner().payment_history()),
            None,
        );
        // Clone the saved state, then release its lock before any network lookup.
        let records = self.saved_store()?.records().to_vec();
        for record in records {
            let psbt = decode_psbt(&record.original_psbt_v2_hex)?;
            for (input, funding) in psbt.unsigned_tx.input.iter().zip(psbt.iter_funding_utxos()) {
                let funding = funding.map_err(|e| format!("Invalid saved funding output: {e}"))?;
                snapshot
                    .prevouts
                    .insert(input.previous_output, funding.clone());
            }
            if !record.finalized_tx_hex.is_empty() {
                let tx: bitcoin::Transaction =
                    deserialize_hex(&record.finalized_tx_hex).map_err(|e| e.to_string())?;
                if !snapshot
                    .history
                    .iter()
                    .any(|summary| summary.txid == tx.compute_txid().to_string())
                {
                    snapshot.history.push(RustTx {
                        txid: tx.compute_txid().to_string(),
                        label: record.label,
                        status: if record.broadcast {
                            "broadcast"
                        } else {
                            "finalized"
                        }
                        .into(),
                        ..RustTx::default()
                    });
                }
                snapshot.transactions.insert(tx.compute_txid(), tx);
            }
        }
        snapshot.add_scanner(inner.account.scanner());
        snapshot.update_history_fees();
        Ok(snapshot)
    }

    pub fn update_tx_label(&self, txid: String, label: String) -> crate::ffi::LabelEditResult {
        self.edit_tx_label(&txid, label).into()
    }

    fn edit_tx_label(&self, txid: &str, label: String) -> Result<(), String> {
        let inner = self.inner.as_ref().ok_or(NOT_INITIALIZED)?;
        let txid = txid.parse::<bitcoin::Txid>().map_err(|e| e.to_string())?;
        crate::tx_details::edit_scanner_label(
            inner.account.scanner(),
            bwk::bwk_electrum::label_store::LabelKey::Transaction(txid),
            label,
        )
    }

    /// Descriptor and derivation path to verify `address` on a signing device. Empty
    /// when `address` is none of the account's.
    pub fn address_verify_info(&self, address: &str) -> AddressVerifyInfo {
        let Some(inner) = &self.inner else {
            return AddressVerifyInfo {
                descriptor: String::new(),
                path: String::new(),
            };
        };
        for entry in inner.account.scanner().address_entries() {
            if entry.address.clone().assume_checked().to_string() != address {
                continue;
            }
            return verify_info(&inner.account.descriptor(), entry.account, entry.index);
        }
        AddressVerifyInfo {
            descriptor: String::new(),
            path: String::new(),
        }
    }

    /// Build a configured TxBuilder from a TransactionTemplate.
    fn build_tx_builder(
        inner: &MusigAccountInner,
        tx_template: &TransactionTemplate,
    ) -> Result<TxBuilder, TemplateError> {
        let network: bitcoin::Network = inner.config.network.into();
        let mut max_addr = None;
        let mut output_total: u64 = 0;
        let mut recipients: Vec<(bitcoin::Address, u64)> = Vec::new();

        for output in &tx_template.outputs {
            let address = parse_recipient(&output.address, network)?;
            if output.max {
                if max_addr.is_some() {
                    return Err(TemplateError::MultipleMax);
                }
                max_addr = Some(address);
            } else {
                output_total += output.amount;
                recipients.push((address, output.amount));
            }
        }

        let feerate_msats_vb =
            crate::account::fee_rate_msats(tx_template.fee_rate, tx_template.fee)?;
        let mut builder = inner.account.tx_builder();
        if tx_template.fee > 0 {
            builder = builder.fee(tx_template.fee);
        } else {
            builder = builder.feerate(feerate_msats_vb);
        }

        for (address, amount) in recipients {
            builder.send_to(address, amount);
        }
        let has_max = max_addr.is_some();
        if let Some(address) = max_addr {
            builder.add_output(Recipient {
                address: address.as_unchecked().clone(),
                amount: Amount::Max(None),
                label: None,
                origin: None,
                descriptor: None,
            });
        }

        if !tx_template.input_outpoints.is_empty() {
            let coins = inner.account.scanner().coins();
            for outpoint_str in &tx_template.input_outpoints {
                let outpoint = parse_outpoint(outpoint_str)?;
                let Some(entry) = coins.get(&outpoint) else {
                    return Err(TemplateError::CoinNotFound(outpoint_str.clone()));
                };
                if matches!(entry.status(), CoinStatus::Spent | CoinStatus::BeingSpend) {
                    return Err(TemplateError::NotSpendable(outpoint_str.clone()));
                }
                builder.add_input(entry.coin.clone());
            }
        } else if has_max {
            builder.drain_inputs();
        } else {
            let coins = builder.select_coins(output_total, feerate_msats_vb);
            if coins.is_empty() {
                return Err(TemplateError::InsufficientFunds);
            }
            for coin in coins {
                builder.add_input(coin);
            }
        }

        Ok(builder)
    }

    /// Simulate a transaction without actually creating it.
    pub fn simulate_transaction(&self, tx_template: TransactionTemplate) -> TransactionSimulation {
        let err_sim = |kind, error: String| TransactionSimulation {
            kind,
            error,
            ..Default::default()
        };

        let Some(inner) = &self.inner else {
            return err_sim(
                crate::ffi::BackendErrorKind::Other,
                NOT_INITIALIZED.to_string(),
            );
        };

        let builder = match Self::build_tx_builder(inner, &tx_template) {
            Ok(b) => b,
            Err(e) => return err_sim(crate::ffi::BackendErrorKind::InvalidInput, e.to_string()),
        };

        crate::tx_preview::simulation_preview(builder.simulate(), tx_template.fee_rate)
    }

    /// Every spend path for the coins `outpoints`: the key path first (left out when the
    /// internal key is unspendable), then leaves in DFS order.
    pub fn spend_leaves(&self, outpoints: Vec<String>) -> MusigSpendLeaves {
        let error = |error: String| MusigSpendLeaves {
            is_ok: false,
            error,
            leaves: Vec::new(),
        };
        let Some(inner) = &self.inner else {
            return error(NOT_INITIALIZED.to_string());
        };
        let outpoints = match outpoints
            .iter()
            .map(|outpoint| parse_outpoint(outpoint))
            .collect::<Result<Vec<_>, _>>()
        {
            Ok(outpoints) => outpoints,
            Err(e) => return error(e.to_string()),
        };
        match inner.account.spend_leaves(&outpoints) {
            Ok(leaves) => MusigSpendLeaves {
                is_ok: true,
                error: String::new(),
                leaves: leaves.into_iter().map(spend_leaf).collect(),
            },
            Err(e) => error(format!("Failed to list spend paths: {e}")),
        }
    }

    /// Prepare the PSBT, then restrict it to `spend` before round 1.
    pub fn prepare_transaction(
        &self,
        tx_template: TransactionTemplate,
        spend: MusigSpendLeaf,
    ) -> Box<PsbtResult> {
        let failed = |error: String| {
            log::error!("prepare_transaction: {error}");
            Box::new(PsbtResult::failed(error))
        };
        let mut active = match self.active.lock() {
            Ok(active) => active,
            Err(error) => return failed(format!("Active attempt lock failed: {error}")),
        };

        let Some(inner) = &self.inner else {
            return Box::new(PsbtResult::failed_kind(
                crate::ffi::BackendErrorKind::Other,
                NOT_INITIALIZED.to_string(),
            ));
        };

        let mut builder = match Self::build_tx_builder(inner, &tx_template) {
            Ok(b) => b,
            Err(e) => return failed(e.to_string()),
        };
        let mut psbt = match builder.generate() {
            Ok(psbt) => psbt,
            Err(e) => return failed(format!("{e:?}")),
        };

        let path = if spend.key_path {
            SpendPath::KeyPath
        } else {
            match TapLeafHash::from_str(&spend.leaf_hash) {
                Ok(leaf_hash) => SpendPath::Leaf(leaf_hash),
                Err(e) => return failed(format!("Invalid leaf hash '{}': {e}", spend.leaf_hash)),
            }
        };
        let outpoints: Vec<_> = psbt
            .unsigned_tx
            .input
            .iter()
            .map(|input| input.previous_output)
            .collect();
        let spend = match inner.account.spend_leaves(&outpoints) {
            Ok(leaves) => match leaves.into_iter().find(|leaf| leaf.path == path) {
                Some(leaf) => spend_leaf(leaf),
                None => {
                    return failed("Selected spend path is not available for these inputs".into())
                }
            },
            Err(error) => return failed(format!("Cannot resolve selected spend path: {error}")),
        };
        if let Err(e) = inner.account.restrict_spend_path(&mut psbt, path) {
            return failed(format!("Failed to restrict the spend path: {e}"));
        }

        if let Err(error) = crate::keys::ledger_nonces::start_attempt(&mut psbt) {
            return failed(error);
        }

        let bytes_v2 = match PsbtV2::from_bitcoin_psbt(psbt.clone()).and_then(|v2| v2.serialize()) {
            Ok(bytes) => bytes,
            Err(e) => return failed(format!("{e:?}")),
        };
        let txid = psbt.unsigned_tx.compute_txid().to_string();
        let mut result = PsbtResult::prepared(psbt.serialize(), bytes_v2.clone(), txid);
        let signers = spend.fingerprints.clone();
        match self
            .saved_store()
            .and_then(|mut store| store.create(tx_template, spend, hex::encode(&bytes_v2)))
        {
            Ok(saved) => {
                result.saved_record_id = saved.record_id;
                result.save_error = saved.save_error;
            }
            Err(error) => {
                result.save_error = format!("Prepared transaction is not persisted: {error}")
            }
        }
        let snapshot = MusigAttemptSnapshot {
            is_ok: true,
            psbt_v2_hex: hex::encode(bytes_v2),
            signers,
            record_id: result.saved_record_id.clone(),
            save_error: result.save_error.clone(),
            ..Default::default()
        };
        *active = Some(snapshot);
        Box::new(result)
    }

    /// Per cosigner nonce and partial signature counts of the PSBT.
    pub fn round_status(&self, psbt_v2_hex: &str) -> MusigRoundStatus {
        let error = |error: String| MusigRoundStatus {
            is_ok: false,
            error,
            round: MusigRound::NotMusig,
            cosigners: Vec::new(),
        };
        let Some(inner) = &self.inner else {
            return error(NOT_INITIALIZED.to_string());
        };
        let psbt = match decode_psbt(psbt_v2_hex) {
            Ok(psbt) => psbt,
            Err(e) => return error(e),
        };
        match inner.account.musig_status(&psbt) {
            Ok(status) => MusigRoundStatus {
                is_ok: true,
                error: String::new(),
                round: match status.round {
                    BwkMusigRound::NotMusig => MusigRound::NotMusig,
                    BwkMusigRound::Nonces => MusigRound::Nonces,
                    BwkMusigRound::PartialSigs => MusigRound::PartialSigs,
                    BwkMusigRound::Ready => MusigRound::Ready,
                },
                cosigners: status
                    .cosigners
                    .into_iter()
                    .map(|cosigner| MusigCosignerStatus {
                        fingerprint: cosigner.fingerprint.to_string(),
                        nonces: cosigner.nonces,
                        partial_sigs: cosigner.partial_sigs,
                        expected: cosigner.expected,
                    })
                    .collect(),
            },
            Err(e) => error(format!("Failed to read the signing round: {e}")),
        }
    }

    /// Aggregate the partial signatures and keep the finalized PSBT alongside the tx.
    pub fn finalize_psbt(&self, psbt_v2_hex: &str) -> MusigFinalizeResult {
        let failed = |error| MusigFinalizeResult {
            error,
            ..MusigFinalizeResult::default()
        };
        let Some(inner) = &self.inner else {
            return failed(NOT_INITIALIZED.to_string());
        };
        let mut psbt = match decode_psbt(psbt_v2_hex) {
            Ok(psbt) => psbt,
            Err(e) => return failed(e),
        };
        let tx = match inner.account.finalize(&mut psbt) {
            Ok(tx) => tx,
            Err(e) => return failed(format!("Failed to finalize PSBT: {e}")),
        };
        let finalized = match PsbtV2::from_bitcoin_psbt(psbt).and_then(|v2| v2.serialize()) {
            Ok(bytes) => hex::encode(bytes),
            Err(e) => return failed(format!("Failed to serialize finalized PSBT: {e:?}")),
        };
        let tx_hex = serialize_hex(&tx);
        let save_error = match self.saved_store() {
            Ok(mut store) => match store.finalized(psbt_v2_hex, &finalized, &tx_hex) {
                Ok(()) => store.error(),
                Err(error) => format!("Finalized transaction is not persisted: {error}"),
            },
            Err(error) => error,
        };
        MusigFinalizeResult {
            is_ok: true,
            error: String::new(),
            psbt_v2_hex: finalized,
            value: tx_hex.clone(),
            tx_hex,
            save_error,
        }
    }

    /// Broadcast a signed transaction via Electrum; the value is the txid.
    pub fn broadcast_transaction(&self, signed_tx_hex: String) -> TxResult {
        let Some(inner) = &self.inner else {
            return TxResult::failed(
                crate::ffi::BackendErrorKind::Other,
                NOT_INITIALIZED.to_string(),
            );
        };
        let tx: bitcoin::Transaction = match deserialize_hex(&signed_tx_hex) {
            Ok(tx) => tx,
            Err(e) => {
                return TxResult::failed(
                    crate::ffi::BackendErrorKind::InvalidInput,
                    format!("Invalid transaction hex: {e}"),
                )
            }
        };
        if let Err(e) = broadcast_via_electrum(
            &inner.config.electrum_url,
            inner.config.verify_certificate,
            &tx,
        ) {
            return TxResult::failed(e.kind, e.message);
        }
        // Reflect the spend locally so the spent coins drop from spendable at once.
        inner.account.scanner().record_unconfirmed_spend(&tx);
        match self.saved_store() {
            Ok(mut store) => {
                if let Err(error) = store.broadcast(&tx) {
                    log::error!("broadcast saved PSBT: {error}");
                }
            }
            Err(error) => log::error!("broadcast saved PSBT: {error}"),
        }
        TxResult {
            is_ok: true,
            kind: crate::ffi::BackendErrorKind::None,
            error: String::new(),
            value: tx.compute_txid().to_string(),
        }
    }

    /// Validate a prepared transaction via Electrum before signing. Blocking.
    pub fn validate_before_sign(&self, psbt_result: &PsbtResult) -> PsbtValidation {
        match &self.inner {
            Some(inner) => psbt_validation(
                psbt_result,
                &inner.config.electrum_url,
                inner.config.verify_certificate,
            ),
            None => PsbtValidation {
                is_ok: false,
                error: NOT_INITIALIZED.to_string(),
                is_valid: false,
                issues: String::new(),
                reused_output_count: 0,
                spent_input_count: 0,
            },
        }
    }

    fn saved_store(&self) -> Result<MutexGuard<'_, SavedPsbtStore>, String> {
        self.inner
            .as_ref()
            .ok_or_else(|| NOT_INITIALIZED.to_string())?
            .saved_psbts
            .lock()
            .map_err(|e| format!("Saved PSBT state lock failed: {e}"))
    }

    fn saved_entry(&self, record: &crate::ffi::SavedPsbtRecord) -> Result<SavedPsbtEntry, String> {
        let inner = self
            .inner
            .as_ref()
            .ok_or_else(|| NOT_INITIALIZED.to_string())?;
        let mut entry = frozen_entry(record)?;
        let status = self.round_status(&record.current_psbt_v2_hex);
        if !status.is_ok {
            return Err(status.error);
        }
        entry.signatures_required = record.spend.fingerprints.len() as u32;
        entry.signatures_known = entry.signatures_required > 0;
        let psbt = decode_psbt(&record.current_psbt_v2_hex)?;
        entry.signatures_complete = record
            .spend
            .fingerprints
            .iter()
            .filter(|fingerprint| {
                if !record.finalized_tx_hex.is_empty() {
                    return true;
                }
                if let Some(cosigner) = status
                    .cosigners
                    .iter()
                    .find(|cosigner| cosigner.fingerprint.eq_ignore_ascii_case(fingerprint))
                {
                    return cosigner.expected > 0 && cosigner.partial_sigs == cosigner.expected;
                }
                bitcoin::bip32::Fingerprint::from_str(fingerprint)
                    .is_ok_and(|fp| ordinary_signature_complete(&psbt, fp))
            })
            .count() as u32;
        apply_chain_state(&mut entry, &inner.account.scanner().tx_history());
        let coins = inner.account.scanner().coins();
        if !entry.in_history
            && entry.warning.is_empty()
            && entry.inputs.iter().any(|input| {
                coins.iter().any(|(outpoint, coin)| {
                    outpoint.to_string() == input.outpoint
                        && matches!(coin.status(), CoinStatus::Spent | CoinStatus::BeingSpend)
                })
            })
        {
            entry.warning =
                "Inputs are marked spent. Signing and broadcast can still be attempted".into();
        }
        Ok(entry)
    }

    pub fn saved_psbts(&self, include_confirmed: bool) -> SavedPsbtList {
        let store = match self.saved_store() {
            Ok(store) => store,
            Err(error) => {
                return SavedPsbtList {
                    error,
                    ..SavedPsbtList::default()
                }
            }
        };
        let mut entries = Vec::new();
        for record in store.records() {
            match self.saved_entry(record) {
                Ok(entry) if include_confirmed || entry.confirmed_height == 0 => {
                    entries.push(entry)
                }
                Ok(_) => (),
                Err(error) => {
                    return SavedPsbtList {
                        error,
                        save_error: store.error(),
                        entries,
                        is_ok: false,
                    }
                }
            }
        }
        entries.sort_by_key(|entry| std::cmp::Reverse(entry.record.created_at));
        SavedPsbtList {
            is_ok: store.load_error().is_empty(),
            error: store.load_error(),
            save_error: store.error(),
            entries,
        }
    }

    pub fn restore_saved_psbt(&self, id: &str) -> SavedPsbtRestore {
        let result = self.saved_store().and_then(|store| {
            self.saved_entry(store.get(id)?)
                .map(|entry| SavedPsbtRestore {
                    is_ok: true,
                    error: String::new(),
                    save_error: store.error(),
                    entry,
                })
        });
        match result {
            Ok(restored) => restored,
            Err(error) => SavedPsbtRestore {
                error,
                ..SavedPsbtRestore::default()
            },
        }
    }

    pub fn update_saved_psbt(&self, id: &str, psbt_v2_hex: &str) -> SavedPsbtMutation {
        self.saved_store()
            .and_then(|mut store| store.update(id, psbt_v2_hex))
            .unwrap_or_else(mutation_error)
    }

    pub fn reset_saved_psbt(&self, id: &str) -> SavedPsbtReset {
        let mut store = match self.saved_store() {
            Ok(store) => store,
            Err(error) => {
                return SavedPsbtReset {
                    error,
                    ..Default::default()
                }
            }
        };
        let result = store.reset(id, |current, baseline| {
            let reset =
                musig_session_restart(self.network(), &self.descriptor(), current, baseline);
            if reset.is_ok {
                Ok(reset.psbt_v2_hex)
            } else {
                Err(reset.error)
            }
        });
        let reset_pending = store.get(id).is_ok_and(|record| record.reset_pending);
        match result {
            Ok(psbt_v2_hex) => SavedPsbtReset {
                is_ok: true,
                psbt_v2_hex,
                save_error: store.error(),
                reset_pending,
                error: String::new(),
            },
            Err(error) => SavedPsbtReset {
                error,
                reset_pending,
                save_error: store.error(),
                ..Default::default()
            },
        }
    }

    pub fn label_saved_psbt(&self, id: &str, label: String) -> SavedPsbtMutation {
        self.saved_store()
            .and_then(|mut store| store.label(id, label))
            .unwrap_or_else(mutation_error)
    }

    pub fn delete_saved_psbt(&self, id: &str) -> SavedPsbtMutation {
        self.saved_store()
            .and_then(|mut store| store.delete(id))
            .unwrap_or_else(mutation_error)
    }

    pub fn retry_saved_psbts(&self) -> SavedPsbtMutation {
        match self.saved_store() {
            Ok(mut store) => SavedPsbtMutation {
                is_ok: true,
                save_error: store.persist(),
                ..SavedPsbtMutation::default()
            },
            Err(error) => mutation_error(error),
        }
    }

    pub fn saved_psbt_error(&self) -> String {
        match self.saved_store() {
            Ok(store) => store.error(),
            Err(error) => error,
        }
    }

    pub fn active_attempt(&self) -> MusigAttemptSnapshot {
        match self.active.lock() {
            Ok(active) => active
                .clone()
                .unwrap_or_else(|| attempt_error("No active attempt".into())),
            Err(error) => attempt_error(format!("Active attempt lock failed: {error}")),
        }
    }

    pub fn active_signing_plan(
        &self,
        devices: Vec<crate::ffi::LiveDevice>,
    ) -> crate::ffi::SigningPlan {
        let snapshot = self.active_attempt();
        if !snapshot.is_ok || snapshot.reset_pending {
            return crate::ffi::SigningPlan {
                is_ok: false,
                error: if snapshot.is_ok {
                    "Active attempt requires reset".into()
                } else {
                    snapshot.error
                },
                candidates: Vec::new(),
                sp_outputs_pending: false,
            };
        }
        let mut plan =
            crate::keys::bridge::musig_session_plan(self.network(), &snapshot.psbt_v2_hex, devices);
        plan.candidates.retain(|candidate| {
            snapshot
                .signers
                .iter()
                .any(|signer| signer.eq_ignore_ascii_case(&candidate.fingerprint))
        });
        plan
    }

    pub fn clear_active_attempt(&self) -> bool {
        match self.active.lock() {
            Ok(mut active) => {
                *active = None;
                true
            }
            Err(error) => {
                log::error!("Active attempt lock failed: {error}");
                false
            }
        }
    }

    pub fn accept_signed_progress(&self, answer: &str) -> MusigAttemptSnapshot {
        let mut guard = match self.active.lock() {
            Ok(guard) => guard,
            Err(error) => return attempt_error(format!("Active attempt lock failed: {error}")),
        };
        let Some(active) = guard.as_mut() else {
            return attempt_error("No active attempt".into());
        };
        if active.reset_pending {
            return attempt_error("Active attempt cannot accept signing progress".into());
        }
        let merged = merge_signed_psbt(&active.psbt_v2_hex, answer);
        if !merged.is_ok {
            return attempt_error(merged.error);
        }
        if !active.tx_hex.is_empty() {
            return active.clone();
        }
        if !active.record_id.is_empty() {
            let saved = self.update_saved_psbt(&active.record_id, &merged.psbt_v2_hex);
            if !saved.is_ok {
                return attempt_error(saved.error);
            }
            active.save_error = saved.save_error;
        }
        active.psbt_v2_hex = merged.psbt_v2_hex;
        self.advance_attempt(active);
        active.clone()
    }

    fn advance_attempt(&self, active: &mut MusigAttemptSnapshot) {
        active.finalize_error.clear();
        if active.reset_pending || !active.tx_hex.is_empty() {
            return;
        }
        let status = self.round_status(&active.psbt_v2_hex);
        if !status.is_ok {
            active.error = status.error;
            active.is_ok = false;
            return;
        }
        let readiness = match status.round {
            MusigRound::Ready => Ok(true),
            MusigRound::NotMusig => self.ordinary_transaction_ready(&active.psbt_v2_hex),
            _ => Ok(false),
        };
        let ready = match readiness {
            Ok(ready) => ready,
            Err(error) => {
                active.finalize_error = error;
                return;
            }
        };
        if !ready {
            return;
        }
        let finalized = self.finalize_psbt(&active.psbt_v2_hex);
        if !finalized.is_ok {
            active.finalize_error = finalized.error;
            return;
        }
        active.psbt_v2_hex = finalized.psbt_v2_hex;
        active.tx_hex = finalized.tx_hex;
        if !active.record_id.is_empty() || !finalized.save_error.is_empty() {
            active.save_error = finalized.save_error;
        }
    }

    fn ordinary_transaction_ready(&self, psbt_v2_hex: &str) -> Result<bool, String> {
        let inner = self.inner.as_ref().ok_or(NOT_INITIALIZED)?;
        let mut psbt = decode_psbt(psbt_v2_hex)?;
        // Trial finalization is pure; candidate progress does not imply policy satisfaction.
        match inner.account.finalize(&mut psbt) {
            Ok(_) => Ok(true),
            Err(bwk::account::Error::Finalize(_)) => Ok(false),
            Err(error) => Err(format!("Failed to check transaction satisfaction: {error}")),
        }
    }

    pub fn finalize_active_attempt(&self) -> MusigAttemptSnapshot {
        let mut guard = match self.active.lock() {
            Ok(guard) => guard,
            Err(error) => return attempt_error(format!("Active attempt lock failed: {error}")),
        };
        let Some(active) = guard.as_mut() else {
            return attempt_error("No active attempt".into());
        };
        self.advance_attempt(active);
        active.clone()
    }

    pub fn activate_saved_psbt(&self, id: &str) -> SavedPsbtRestore {
        let mut active = match self.active.lock() {
            Ok(active) => active,
            Err(error) => {
                return SavedPsbtRestore {
                    error: format!("Active attempt lock failed: {error}"),
                    ..Default::default()
                }
            }
        };
        let mut restored = self.restore_saved_psbt(id);
        if !restored.is_ok {
            return restored;
        }
        let record = &restored.entry.record;
        let mut snapshot = MusigAttemptSnapshot {
            is_ok: true,
            psbt_v2_hex: record.current_psbt_v2_hex.clone(),
            tx_hex: record.finalized_tx_hex.clone(),
            record_id: record.id.clone(),
            signers: record.spend.fingerprints.clone(),
            reset_pending: record.reset_pending,
            save_error: restored.save_error.clone(),
            ..Default::default()
        };
        self.advance_attempt(&mut snapshot);
        restored.is_ok = snapshot.is_ok;
        restored.error = snapshot.error.clone();
        restored.entry.record.current_psbt_v2_hex = snapshot.psbt_v2_hex.clone();
        restored.entry.record.finalized_tx_hex = snapshot.tx_hex.clone();
        restored.save_error = snapshot.save_error.clone();
        *active = Some(snapshot);
        restored
    }

    pub fn reset_active_attempt(&self) -> MusigAttemptSnapshot {
        let mut guard = match self.active.lock() {
            Ok(guard) => guard,
            Err(error) => return attempt_error(format!("Active attempt lock failed: {error}")),
        };
        let Some(active) = guard.as_mut() else {
            return attempt_error("No active attempt".into());
        };
        let reset = self.reset_saved_psbt(&active.record_id);
        if !reset.is_ok {
            active.reset_pending |= reset.reset_pending;
            if !reset.save_error.is_empty() {
                active.save_error = reset.save_error;
            }
            let mut failure = active.clone();
            failure.is_ok = false;
            failure.error = reset.error;
            return failure;
        }
        active.reset_pending = reset.reset_pending;
        active.save_error = reset.save_error;
        active.psbt_v2_hex = reset.psbt_v2_hex;
        active.tx_hex.clear();
        active.finalize_error.clear();
        active.clone()
    }

    pub fn require_active_restart(&self) -> MusigAttemptSnapshot {
        match self.active.lock() {
            Ok(mut guard) => match guard.as_mut() {
                Some(active) => {
                    if active.tx_hex.is_empty() {
                        active.reset_pending = true;
                    }
                    active.clone()
                }
                None => attempt_error("No active attempt".into()),
            },
            Err(error) => attempt_error(format!("Active attempt lock failed: {error}")),
        }
    }
}

fn attempt_error(error: String) -> MusigAttemptSnapshot {
    MusigAttemptSnapshot {
        error,
        ..Default::default()
    }
}

/// A recipient as a bitcoin address of `network`; a silent payment address is refused.
fn parse_recipient(
    address: &str,
    network: bitcoin::Network,
) -> Result<bitcoin::Address, TemplateError> {
    if let Ok(RecipientAddress::SpAddress(_)) = RecipientAddress::parse(address, network) {
        return Err(TemplateError::Address(
            address.to_string(),
            SP_RECIPIENT_UNSUPPORTED.to_string(),
        ));
    }
    bitcoin::Address::from_str(address)
        .and_then(|address| address.require_network(network))
        .map_err(|e| TemplateError::Address(address.to_string(), e.to_string()))
}

/// What IKeys::verifyAddress needs for the address at `keychain`/`index` of `descriptor`.
fn verify_info(descriptor: &Descriptor, keychain: KeyChain, index: u32) -> AddressVerifyInfo {
    let branch: u32 = match keychain {
        KeyChain::Receive => 0,
        KeyChain::Change => 1,
        KeyChain::Custom(c) => c,
    };
    AddressVerifyInfo {
        descriptor: descriptor.to_string(),
        path: format!("{branch}/{index}"),
    }
}

fn spend_leaf(leaf: SpendLeaf) -> MusigSpendLeaf {
    let (key_path, leaf_hash) = match leaf.path {
        SpendPath::KeyPath => (true, String::new()),
        SpendPath::Leaf(leaf_hash) => (false, leaf_hash.to_string()),
    };
    MusigSpendLeaf {
        key_path,
        leaf_hash,
        fingerprints: leaf.signers.iter().map(ToString::to_string).collect(),
        relative: u32::from(leaf.relative.unwrap_or(0)),
        absolute: leaf.absolute.unwrap_or(0),
        spendable: leaf.spendable,
        satisfaction_wu: leaf.satisfaction_wu,
    }
}

/// A hex PSBT v2 as the v0 PSBT bwk's account calls take.
pub(crate) fn decode_psbt(psbt_v2_hex: &str) -> Result<bitcoin::Psbt, String> {
    let bytes = hex::decode(psbt_v2_hex.trim()).map_err(|e| format!("Invalid PSBT hex: {e}"))?;
    let psbt = crate::keys::psbt::parse_v2(&bytes).map_err(|e| format!("Invalid PSBT: {e}"))?;
    psbt.into_bitcoin_psbt()
        .map_err(|e| format!("Invalid PSBT: {e:?}"))
}

#[cfg(test)]
mod tests {
    use std::{str::FromStr, sync::Mutex};

    use bitcoin::{
        absolute::LockTime,
        bip32::{ChildNumber, DerivationPath, Xpriv, Xpub},
        consensus::encode::deserialize_hex,
        ecdsa,
        hashes::Hash,
        secp256k1::{Message, Secp256k1},
        sighash::{EcdsaSighashType, SighashCache},
        transaction::Version,
        Amount, OutPoint, Psbt, PublicKey, Transaction, TxIn, TxOut, Txid,
    };
    use bwk::{
        bwk_descriptor::descriptor::Descriptor, bwk_sign::hot_signer::HotSigner,
        bwk_tx::bwk_psbt::PsbtV2,
    };
    use temp_dir::TempDir;

    use crate::{
        config::Config,
        ffi::{MusigAttemptSnapshot, Network, Output, TransactionTemplate},
        musig::MusigAccount,
        state_crypt,
    };

    struct UnsavedAttempt {
        _directory: TempDir,
        account: MusigAccount,
        descriptor: Descriptor,
        signer: HotSigner,
        psbt: PsbtV2,
    }

    fn offline_account(config: Config) -> MusigAccount {
        MusigAccount {
            inner: Some(MusigAccount::new_inner_connected(config, false).expect("account")),
            error: String::new(),
            closed_saved_psbts: None,
            closed_name: None,
            pending_flush_error: String::new(),
            active: Mutex::new(None),
        }
    }

    fn unsaved_attempt() -> UnsavedAttempt {
        state_crypt::unlock_for_tests();
        let directory = TempDir::new().expect("isolated no-record account");
        let secp = Secp256k1::new();
        let master = Xpriv::new_master(bitcoin::Network::Regtest, &[73; 32]).expect("master");
        let descriptor = format!(
            "tr([{}]{}/<0;1>/*)",
            master.fingerprint(&secp),
            Xpub::from_priv(&secp, &master)
        );
        let mut config = Config::new(
            format!("no_record_{}", std::process::id()),
            Network::Regtest,
            descriptor.clone(),
            String::new(),
            String::new(),
            None,
        );
        config.plugin_id = "musig".into();
        config.data_dir = directory.path().to_path_buf();
        let mut seeded = offline_account(config.clone());
        let backend = &mut seeded.inner.as_mut().expect("seed backend").account;
        let receive = backend.scanner_mut().new_recv_addr();
        let funding = Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: vec![TxIn {
                previous_output: OutPoint::new(Txid::from_byte_array([74; 32]), 0),
                ..Default::default()
            }],
            output: vec![TxOut {
                value: Amount::from_sat(100_000),
                script_pubkey: receive.script_pubkey(),
            }],
        };
        backend.scanner().record_unconfirmed_spend(&funding);
        assert!(
            seeded.shutdown_backend().is_empty(),
            "seed wallet state is durable"
        );
        drop(seeded);
        let mut account = offline_account(config);
        let outpoint = OutPoint::new(funding.compute_txid(), 0).to_string();
        let leaves = account.spend_leaves(vec![outpoint.clone()]);
        assert!(leaves.is_ok, "spend paths: {}", leaves.error);
        let recipient = account.new_addr();
        let prepared = account.prepare_transaction(
            TransactionTemplate {
                outputs: vec![Output {
                    address: recipient,
                    amount: 40_000,
                    max: false,
                    label: String::new(),
                }],
                fee: 1000,
                fee_rate: 0.0,
                input_outpoints: vec![outpoint],
            },
            leaves.leaves[0].clone(),
        );
        assert!(
            prepared.is_ok(),
            "prepared bytes: {}",
            prepared.get_psbt_error()
        );
        {
            let mut guard = account.active.lock().expect("active lock");
            let active = guard.as_mut().expect("prepared active attempt");
            active.record_id.clear();
            active.save_error = "Prepared transaction is not persisted".into();
        }
        let parsed = Descriptor::from_str(&descriptor).expect("descriptor");
        let psbt =
            PsbtV2::deserialize(&hex::decode(prepared.bytes_v2_hex()).expect("hex")).expect("PSBT");
        UnsavedAttempt {
            _directory: directory,
            account,
            descriptor: parsed,
            signer: HotSigner::new_from_xpriv(bitcoin::Network::Regtest, master),
            psbt,
        }
    }

    #[test]
    fn active_attempt_without_record_id_retains_signed_transaction() {
        let mut fixture = unsaved_attempt();
        fixture
            .signer
            .sign_v2(
                &mut fixture.psbt,
                fixture.descriptor.as_miniscript().expect("policy"),
            )
            .expect("ordinary signature");
        let snapshot = fixture
            .account
            .accept_signed_progress(&hex::encode(fixture.psbt.serialize().expect("signed PSBT")));
        assert!(
            snapshot.is_ok,
            "unsaved progress accepted: {}",
            snapshot.error
        );
        assert!(
            snapshot.record_id.is_empty(),
            "no fabricated saved record identity"
        );
        assert!(!snapshot.tx_hex.is_empty(), "unsaved progress can finalize");
        assert!(
            !snapshot.save_error.is_empty(),
            "unsaved finalized progress carries a warning"
        );
        assert_eq!(
            fixture.account.active_attempt().tx_hex,
            snapshot.tx_hex,
            "usable transaction remains in RAM"
        );
    }

    #[test]
    fn failed_reset_without_record_id_keeps_required_restart_fence() {
        let mut fixture = unsaved_attempt();
        fixture
            .signer
            .sign_v2(
                &mut fixture.psbt,
                fixture.descriptor.as_miniscript().expect("policy"),
            )
            .expect("real signature for the prepared transaction");
        let answer = hex::encode(fixture.psbt.serialize().expect("signed PSBT"));
        let required = fixture.account.require_active_restart();
        assert!(
            required.reset_pending && required.record_id.is_empty(),
            "restart required for an unsaved attempt"
        );
        let failed = fixture.account.reset_active_attempt();
        assert!(
            !failed.is_ok && failed.reset_pending,
            "failed reset without identity retains the fence: {}",
            failed.error
        );
        let current = fixture.account.active_attempt();
        assert!(
            current.reset_pending,
            "authoritative Rust fence remains set"
        );
        assert_eq!(
            current.save_error, required.save_error,
            "failed reset cannot erase the unsaved-progress warning"
        );
        let plan = fixture.account.active_signing_plan(Vec::new());
        assert!(!plan.is_ok, "Rust signing plan refuses the fenced attempt");
        assert_eq!(
            plan.error, "Active attempt requires reset",
            "plan refusal is independent of Qt"
        );
        let refused = fixture.account.accept_signed_progress(&answer);
        assert!(
            !refused.is_ok,
            "valid signed bytes cannot bypass the restart fence"
        );
        assert_eq!(
            refused.error, "Active attempt cannot accept signing progress",
            "acceptance is fenced before merge"
        );
        assert_eq!(
            fixture.account.active_attempt().psbt_v2_hex,
            required.psbt_v2_hex,
            "fenced attempt retains its original progress"
        );
        assert!(
            fixture.account.active_attempt().tx_hex.is_empty(),
            "fenced progress was not finalized"
        );
    }

    #[test]
    fn ordinary_p2pkh_readiness_uses_a_real_signed_psbt() {
        state_crypt::unlock_for_tests();
        let directory = TempDir::new().expect("isolated P2PKH account");
        let secp = Secp256k1::new();
        let master = Xpriv::new_master(bitcoin::Network::Regtest, &[211; 32]).expect("master");
        let fingerprint = master.fingerprint(&secp);
        let descriptor = format!(
            "pkh([{fingerprint}]{}/<0;1>/*)",
            Xpub::from_priv(&secp, &master)
        );
        assert!(
            crate::musig::validate_musig_descriptor(descriptor.clone()).is_ok,
            "P2PKH descriptor is accepted"
        );
        let mut config = Config::new(
            format!("p2pkh_{}", std::process::id()),
            Network::Regtest,
            descriptor,
            String::new(),
            String::new(),
            None,
        );
        config.plugin_id = "musig".into();
        config.data_dir = directory.path().to_path_buf();
        let account = offline_account(config);
        let child = ChildNumber::from_normal_idx(0).expect("normal child");
        let path = DerivationPath::from(vec![child, child]);
        let key = master.derive_priv(&secp, &path).expect("spending key");
        let public = PublicKey::new(key.private_key.public_key(&secp));
        let output = TxOut {
            value: Amount::from_sat(100_000),
            script_pubkey: bitcoin::ScriptBuf::new_p2pkh(&public.pubkey_hash()),
        };
        let funding = Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: vec![TxIn {
                previous_output: OutPoint::new(Txid::from_byte_array([212; 32]), 0),
                ..Default::default()
            }],
            output: vec![output.clone()],
        };
        let transaction = Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: vec![TxIn {
                previous_output: OutPoint::new(funding.compute_txid(), 0),
                ..Default::default()
            }],
            output: vec![TxOut {
                value: Amount::from_sat(99_000),
                script_pubkey: output.script_pubkey.clone(),
            }],
        };
        // bwk's builder rejects P2PKH inputs; provide a real legacy PSBT with its funding transaction.
        let mut psbt = Psbt::from_unsigned_tx(transaction).expect("legacy PSBT");
        psbt.inputs[0].non_witness_utxo = Some(funding);
        psbt.inputs[0].witness_utxo = Some(output.clone());
        psbt.inputs[0]
            .bip32_derivation
            .insert(public.inner, (fingerprint, path));
        crate::keys::ledger_nonces::start_attempt(&mut psbt).expect("attempt identity");
        let prepared = hex::encode(
            PsbtV2::from_bitcoin_psbt(psbt.clone())
                .expect("PSBT v2")
                .serialize()
                .expect("prepared bytes"),
        );
        *account.active.lock().expect("active lock") = Some(MusigAttemptSnapshot {
            is_ok: true,
            psbt_v2_hex: prepared,
            signers: vec![fingerprint.to_string()],
            save_error: "Prepared transaction is not persisted".into(),
            ..Default::default()
        });
        assert!(
            account.finalize_active_attempt().tx_hex.is_empty(),
            "unsigned legacy policy is not ready"
        );
        let hash = SighashCache::new(&psbt.unsigned_tx)
            .legacy_signature_hash(0, &output.script_pubkey, EcdsaSighashType::All.to_u32())
            .expect("legacy sighash");
        let message = Message::from_digest(hash.to_byte_array());
        let signature = secp.sign_ecdsa(&message, &key.private_key);
        secp.verify_ecdsa(&message, &signature, &public.inner)
            .expect("real ECDSA signature");
        psbt.inputs[0].partial_sigs.insert(
            public,
            ecdsa::Signature {
                signature,
                sighash_type: EcdsaSighashType::All,
            },
        );
        let answer = hex::encode(
            PsbtV2::from_bitcoin_psbt(psbt)
                .expect("signed v2")
                .serialize()
                .expect("signed bytes"),
        );
        let accepted = account.accept_signed_progress(&answer);
        assert!(
            accepted.is_ok && accepted.finalize_error.is_empty(),
            "legacy satisfaction: {} {}",
            accepted.error,
            accepted.finalize_error
        );
        assert!(
            !accepted.tx_hex.is_empty(),
            "accepted P2PKH signature finalizes without taproot progress fields"
        );
        let finalized: Transaction =
            deserialize_hex(&accepted.tx_hex).expect("finalized legacy transaction");
        assert!(
            !finalized.input[0].script_sig.is_empty() && finalized.input[0].witness.is_empty(),
            "actual P2PKH scriptSig satisfaction"
        );
    }
}
