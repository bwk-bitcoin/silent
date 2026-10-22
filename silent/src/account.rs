//! Account module for Silent.
//!
//! Wraps bwk-sp::Account with CXX-compatible interface for C++ bindings.

use std::{
    sync::{mpsc, Arc},
    time::Duration,
};

use bwk_sp::account::recipient::SpRecipientAddress;
use bwk_sp::account::{Account as SpAccount, ScanMode};
use bwk_sp::bwk::bwk_coin::KeyChain;
use bwk_sp::bwk::bwk_electrum::{
    coin_store::{Payment, PaymentType},
    notification::{Notification as BwkNotification, SpNotification, TxListenerNotif},
};
use bwk_sp::bwk_sign::bwk_descriptor::wallet::WalletDescriptor;
use bwk_sp::bwk_sign::miniscript::Descriptor as MiniscriptDescriptor;
use bwk_sp::receiver::RecipientAddress;

use crate::{
    account_storage::Factory,
    config::Config,
    error::BackendFailure,
    ffi::{
        AddressVerifyInfo, BackendErrorKind, CoinState, Notification, NotificationFlag, RustCoin,
        RustTx, RustTxDetails, TransactionSimulation, TransactionTemplate, TxResult,
    },
    tx_details::{details_result, TxSnapshot},
};

pub(crate) const MSATS_PER_SAT: u64 = 1000;

/// Valid account state (created successfully).
struct AccountInner {
    account: SpAccount,
    factory: Arc<Factory>,
    /// The account's name: bwk only knows its opaque directory name.
    name: String,
    receiver: Option<mpsc::Receiver<BwkNotification>>,
    network: bitcoin::Network,
    electrum_url: String,
    verify_certificate: bool,
}

/// Account wrapping bwk-sp::Account.
/// Always constructible; check is_ok() before use.
pub struct Account {
    inner: Option<AccountInner>,
    error: String,
    closed_name: Option<String>,
    pending_flush_error: String,
}

/// The script of a sub-account, read off its descriptor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SubAccountScript {
    Segwit,
    Taproot,
}

impl SubAccountScript {
    fn of(descriptor: &WalletDescriptor) -> Option<Self> {
        match descriptor {
            MiniscriptDescriptor::Wpkh(_) => Some(Self::Segwit),
            MiniscriptDescriptor::Tr(_) => Some(Self::Taproot),
            _ => None,
        }
    }
}

impl Account {
    /// Create a new Account from config (for Rust-side use / tests).
    pub fn new(config: Config) -> Result<Self, String> {
        let inner = Self::new_inner(config)?;
        Ok(Account {
            inner: Some(inner),
            error: String::new(),
            closed_name: None,
            pending_flush_error: String::new(),
        })
    }

    /// Create a new AccountInner from config.
    fn new_inner(config: Config) -> Result<AccountInner, String> {
        Self::new_inner_connected(config, true)
    }

    fn new_inner_connected(config: Config, connect: bool) -> Result<AccountInner, String> {
        let network = match config.network {
            crate::ffi::Network::Regtest => bitcoin::Network::Regtest,
            crate::ffi::Network::Signet => bitcoin::Network::Signet,
            crate::ffi::Network::Testnet => bitcoin::Network::Testnet,
            crate::ffi::Network::Bitcoin => bitcoin::Network::Bitcoin,
            _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
        };

        let mut runtime = config.clone();
        if !connect {
            runtime.electrum_url.clear();
        }
        let sp_config = runtime.to_sp_config()?;
        let factory = Factory::open(&config)?;
        let config_store = factory.config_store();
        let mut account = SpAccount::with_backend_factory(
            sp_config,
            factory.clone(),
            config_store,
            None,
            Default::default(),
        )
        .map_err(|e| e.to_string())?;
        let receiver = account.receiver();

        Ok(AccountInner {
            account,
            factory,
            name: config.account_name.clone(),
            receiver,
            network,
            electrum_url: config.electrum_url.clone(),
            verify_certificate: config.verify_certificate,
        })
    }

    pub fn shutdown_backend(&mut self) -> String {
        let Some(mut old) = self.inner.take() else {
            return self.pending_flush_error.clone();
        };
        old.account.stop_scanner();
        old.account.stop_electrum();
        let error = old
            .account
            .try_persist()
            .err()
            .map(|e| e.to_string())
            .unwrap_or_default();
        old.factory.begin_shutdown();
        drop(old.account);
        if let Some(receiver) = old.receiver {
            while receiver.recv().is_ok() {}
        }
        old.factory.wait_for_backends();
        let late_error = old.factory.failure();
        if !self.pending_flush_error.is_empty() {
            self.pending_flush_error.clone()
        } else if error.is_empty() {
            late_error
        } else {
            error
        }
    }

    /// Header workers can outlive account drop; drain the taken receiver before reopening.
    pub fn close_backend(&mut self) {
        if let Some(mut old) = self.inner.take() {
            self.closed_name = Some(old.name);
            old.account.stop_scanner();
            old.account.stop_electrum();
            if let Err(error) = old.account.try_persist() {
                self.pending_flush_error = error.to_string();
            }
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
            .map(|inner| inner.name.as_str())
            .or(self.closed_name.as_deref())
    }

    /// Called after workers and the old notification receiver have drained.
    pub fn reopen_backend(&mut self, connect_electrum: bool) -> crate::ffi::EndpointResult {
        self.close_backend();
        if !self.pending_flush_error.is_empty() {
            return crate::ffi::EndpointResult {
                kind: BackendErrorKind::Storage,
                error: self.pending_flush_error.clone(),
                ..Default::default()
            };
        }
        let mut failure_kind = BackendErrorKind::Other;
        let result = (|| {
            let name = self
                .closed_name
                .as_ref()
                .ok_or("Account not initialized")?
                .clone();
            failure_kind = BackendErrorKind::Storage;
            let config = Config::from_file(name).map_err(|e| e.to_string())?;
            failure_kind = BackendErrorKind::InvalidInput;
            config.to_sp_config()?;
            failure_kind = BackendErrorKind::Other;
            let usable = matches!(
                crate::config::parse_electrum_url(&config.electrum_url),
                (Some(_), Some(_))
            );
            let mut connection_error = None;
            self.inner = Some(
                match Self::new_inner_connected(config.clone(), connect_electrum && usable) {
                    Ok(inner) => inner,
                    Err(error) if connect_electrum && usable => {
                        connection_error =
                            Some(format!("Cannot connect; backend reopened offline: {error}"));
                        Self::new_inner_connected(config.clone(), false)?
                    }
                    Err(error) => return Err(error),
                },
            );
            self.closed_name = None;
            self.error.clear();
            Ok(crate::ffi::EndpointResult {
                connection_pending: connect_electrum && usable && connection_error.is_none(),
                kind: if connection_error.is_some() || (connect_electrum && !usable) {
                    BackendErrorKind::Electrum
                } else {
                    BackendErrorKind::None
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

    /// The account's network, Regtest when it failed to load: check `is_ok()` first.
    pub fn network(&self) -> crate::ffi::Network {
        match &self.inner {
            Some(inner) => inner.network.into(),
            None => crate::ffi::Network::Regtest,
        }
    }

    /// Check if the account was created successfully.
    pub fn is_ok(&self) -> bool {
        self.inner.is_some()
    }

    /// Get error message (empty if is_ok).
    pub fn get_error(&self) -> String {
        if self.pending_flush_error.is_empty() {
            self.error.clone()
        } else {
            self.pending_flush_error.clone()
        }
    }

    pub fn electrum_url(&self) -> String {
        self.inner
            .as_ref()
            .map(|inner| inner.electrum_url.clone())
            .unwrap_or_default()
    }

    pub fn blindbit_url(&self) -> String {
        self.inner
            .as_ref()
            .map(|inner| inner.account.blindbit_url().to_string())
            .unwrap_or_default()
    }

    pub fn verify_certificate(&self) -> bool {
        self.inner
            .as_ref()
            .is_none_or(|inner| inner.verify_certificate)
    }

    pub fn electrum_running(&self) -> bool {
        let Some(inner) = &self.inner else {
            return false;
        };
        if inner.account.scanners().next().is_some() {
            inner.account.scanners().all(|scanner| scanner.online())
        } else {
            inner
                .account
                .header_store()
                .is_some_and(|headers| headers.running())
        }
    }

    pub(crate) fn scanner_running(&self) -> bool {
        self.inner
            .as_ref()
            .is_some_and(|inner| inner.account.scanner_running())
    }

    /// Start the scanner in continuous mode.
    pub fn start_scanner(&mut self) -> bool {
        let Some(inner) = &mut self.inner else {
            return false;
        };
        match inner.account.start_scan(ScanMode::Continuous, None) {
            Ok(()) => true,
            Err(e) => {
                log::error!("scanner error: {e}");
                false
            }
        }
    }

    /// Stop the scanner.
    pub fn stop_scanner(&mut self) {
        if let Some(inner) = &mut self.inner {
            inner.account.stop_scan();
        }
    }

    /// Stop all electrum listeners on sub-accounts.
    pub fn stop_electrum(&mut self) {
        if let Some(inner) = &mut self.inner {
            inner.account.stop_electrum();
        }
    }

    /// Reload electrum config from file, apply new settings, and start listeners.
    pub fn start_electrum(&mut self) -> bool {
        let Some(inner) = &mut self.inner else {
            return false;
        };

        let config = match crate::config::Config::from_file(inner.name.clone()) {
            Ok(c) => c,
            Err(e) => {
                log::error!("start_electrum: failed to load config: {e}");
                return false;
            }
        };

        if config.electrum_url != inner.electrum_url
            || config.verify_certificate != inner.verify_certificate
            || inner.account.get_config().endpoint().url().is_none()
        {
            log::error!("start_electrum: endpoint changed, reopen the backend first");
            return false;
        }
        inner.account.start_electrum();
        true
    }

    /// Try to receive a notification (non-blocking).
    pub fn try_recv(&mut self) -> Box<Poll> {
        let Some(inner) = &mut self.inner else {
            return Box::new(Poll::err("Account not initialized"));
        };
        if let Some(ref receiver) = inner.receiver {
            match receiver.try_recv() {
                Ok(notif) => {
                    let notification = convert_notification(notif);
                    Box::new(Poll::ok(notification))
                }
                Err(mpsc::TryRecvError::Empty) => Box::new(Poll::none()),
                Err(mpsc::TryRecvError::Disconnected) => {
                    Box::new(Poll::err("Notification channel disconnected"))
                }
            }
        } else {
            Box::new(Poll::err("No notification receiver"))
        }
    }

    /// Take ownership of the notification receiver for use in a dedicated thread.
    /// Can only be called once; subsequent calls return a disconnected receiver.
    pub fn take_receiver(&mut self) -> Box<NotificationReceiver> {
        let Some(inner) = self.inner.as_mut() else {
            log::error!("take_receiver: Account not initialized");
            let (_tx, rx) = mpsc::channel();
            return Box::new(NotificationReceiver { receiver: rx });
        };
        let Some(receiver) = inner.receiver.take() else {
            log::error!("take_receiver: Notification receiver already taken");
            let (_tx, rx) = mpsc::channel();
            return Box::new(NotificationReceiver { receiver: rx });
        };
        Box::new(NotificationReceiver { receiver })
    }

    /// Get account name.
    pub fn name(&self) -> String {
        match &self.inner {
            Some(inner) => inner.name.clone(),
            None => String::new(),
        }
    }

    /// Get balance in satoshis (SP + sub-accounts).
    pub fn balance(&self) -> u64 {
        match &self.inner {
            Some(inner) => inner.account.total_balance(),
            None => 0,
        }
    }

    /// Get the Silent Payment address for this account.
    pub fn sp_address(&self) -> String {
        match &self.inner {
            Some(inner) => inner.account.sp_address().to_string(),
            None => String::new(),
        }
    }

    /// Check if the account has sub-accounts (segwit + taproot).
    pub fn has_sub_accounts(&self) -> bool {
        match &self.inner {
            Some(inner) => inner.account.scanners().count() >= 2,
            None => false,
        }
    }

    /// Get the number of sub-accounts.
    pub fn sub_account_count(&self) -> u32 {
        match &self.inner {
            Some(inner) => inner.account.scanners().count() as u32,
            None => 0,
        }
    }

    /// Whether the account has a segwit (wpkh) sub-account.
    pub fn has_segwit(&self) -> bool {
        self.sub_account_index(SubAccountScript::Segwit).is_some()
    }

    /// Whether the account has a taproot (tr) sub-account.
    pub fn has_taproot(&self) -> bool {
        self.sub_account_index(SubAccountScript::Taproot).is_some()
    }

    /// Generate a new segwit (wpkh) receiving address, empty without that sub-account.
    pub fn new_segwit_addr(&mut self) -> String {
        self.new_sub_account_addr(SubAccountScript::Segwit)
    }

    /// Generate a new taproot receiving address, empty without that sub-account.
    pub fn new_taproot_addr(&mut self) -> String {
        self.new_sub_account_addr(SubAccountScript::Taproot)
    }

    /// The sub-account of that script, found by its descriptor: an account may
    /// have either one alone.
    fn sub_account_index(&self, script: SubAccountScript) -> Option<usize> {
        let inner = self.inner.as_ref()?;
        inner
            .account
            .scanners()
            .position(|scanner| SubAccountScript::of(&scanner.descriptor()) == Some(script))
    }

    fn new_sub_account_addr(&mut self, script: SubAccountScript) -> String {
        let Some(index) = self.sub_account_index(script) else {
            return String::new();
        };
        let Some(inner) = &mut self.inner else {
            return String::new();
        };
        let Some(scanner) = inner.account.scanners_mut().nth(index) else {
            return String::new();
        };
        scanner.new_addr().address.assume_checked().to_string()
    }

    /// Get all coins (both spent and unspent, SP + sub-accounts).
    pub fn coins(&self) -> Vec<RustCoin> {
        use bwk_sp::bwk::bwk_electrum::notification::CoinOrigin;

        let Some(inner) = &self.inner else {
            return vec![];
        };
        let scripts: Vec<Option<SubAccountScript>> = inner
            .account
            .scanners()
            .map(|scanner| SubAccountScript::of(&scanner.descriptor()))
            .collect();
        let history = inner.account.payment_history();
        inner
            .account
            .all_coins()
            .into_values()
            .map(|coin| {
                let account_type = match coin.origin {
                    CoinOrigin::Sp => "SP",
                    CoinOrigin::SubAccount { index, .. } => {
                        match scripts.get(index).copied().flatten() {
                            Some(SubAccountScript::Segwit) => "Segwit",
                            Some(SubAccountScript::Taproot) => "Taproot",
                            None => "Unknown",
                        }
                    }
                }
                .to_string();
                RustCoin {
                    outpoint: format!("{}:{}", coin.outpoint.txid, coin.outpoint.vout),
                    value: coin.amount.to_sat(),
                    height: coin.height.unwrap_or(0),
                    timestamp: history
                        .iter()
                        .find(|payment| payment.txid == coin.outpoint.txid.to_string())
                        .and_then(|payment| payment.timestamp)
                        .unwrap_or(0),
                    label: coin.label.unwrap_or_default(),
                    spent: !coin.spendable,
                    account_type,
                }
            })
            .collect()
    }

    pub fn spending_coins(&self) -> crate::ffi::SpendCoinsResult {
        self.export_spending_coins().into()
    }

    fn export_spending_coins(&self) -> Result<Vec<crate::ffi::RustSpendCoin>, String> {
        let inner = self.inner.as_ref().ok_or("Account not initialized")?;
        let network = inner.network.into();
        let account_index =
            bitcoin::bip32::ChildNumber::from_hardened_idx(0).map_err(|e| e.to_string())?;
        let source = inner.account.mnemonic_fingerprint().map(|fingerprint| {
            (
                fingerprint,
                bwk_sp::receiver::spend_path(inner.network, account_index),
            )
        });
        let spend_pk = inner.account.sp_receiver().spend_pubkey();
        let mut coins = Vec::new();
        for (outpoint, entry) in inner.account.coins() {
            let mut coin = bwk_sp::account::sp_coin_entry_to_coin(outpoint, &entry);
            coin.label = inner.account.get_coin_label(&outpoint);
            coins.push(crate::spending::sp_coin(
                &coin,
                network,
                spend_pk,
                source.clone(),
            )?);
        }
        for scanner in inner.account.scanners() {
            let history = scanner.tx_history();
            for entry in scanner.coins().into_values() {
                let mut coin = entry.coin;
                coin.label = inner.account.get_coin_label(&coin.outpoint);
                let mut exported = crate::spending::descriptor_coin(&coin, network)?;
                let coinbase = history
                    .iter()
                    .find(|tx| tx.txid() == coin.outpoint.txid)
                    .map(|tx| tx.tx().is_coinbase());
                crate::spending::set_availability(&mut exported, coinbase);
                coins.push(exported);
            }
        }
        Ok(coins)
    }

    /// Get spendable coins summary (SP + sub-accounts).
    pub fn spendable_coins(&self) -> CoinState {
        match &self.inner {
            Some(inner) => {
                let summary = inner.account.all_spendable_coins();
                CoinState {
                    confirmed_count: summary.confirmed_count,
                    confirmed_balance: summary.confirmed_balance.to_sat(),
                    unconfirmed_count: summary.unconfirmed_count,
                    unconfirmed_balance: summary.unconfirmed_balance.to_sat(),
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

    /// Update the label for a coin.
    pub fn update_coin_label(
        &self,
        outpoint: String,
        label: String,
    ) -> crate::ffi::LabelEditResult {
        self.edit_coin_label(&outpoint, label).into()
    }

    fn edit_coin_label(&self, outpoint: &str, label: String) -> Result<(), String> {
        let inner = self.inner.as_ref().ok_or("Account not initialized")?;
        let outpoint = parse_outpoint(outpoint).map_err(|e| e.to_string())?;
        inner
            .account
            .try_update_coin_label(outpoint, label)
            .map_err(|e| format!("Label is not saved: {e}. Edit it again to retry."))
    }

    /// Get payment history (SP + sub-accounts), newest first.
    pub fn payment_history(&self) -> Vec<RustTx> {
        match self.tx_snapshot() {
            Ok(snapshot) => snapshot.history,
            Err(error) => {
                log::error!("payment_history: {error}");
                Vec::new()
            }
        }
    }

    pub fn tx_details(&self, txid: &str, allow_network: bool) -> RustTxDetails {
        details_result(self.tx_snapshot().and_then(|snapshot| {
            let inner = self.inner.as_ref().ok_or("Account not initialized")?;
            let mut details = snapshot.details(
                txid,
                &inner.electrum_url,
                inner.verify_certificate,
                allow_network,
            )?;
            for scanner in inner.account.scanners() {
                crate::tx_details::apply_scanner_labels(&mut details, scanner)?;
            }
            for io in details.inputs.iter_mut().chain(details.outputs.iter_mut()) {
                let outpoint = parse_outpoint(&io.outpoint).map_err(|e| e.to_string())?;
                if let Some(label) = inner.account.get_coin_label(&outpoint) {
                    io.label = label;
                }
            }
            Ok(details)
        }))
    }

    fn tx_snapshot(&self) -> Result<TxSnapshot, String> {
        let inner = self.inner.as_ref().ok_or("Account not initialized")?;
        let mut snapshot = TxSnapshot::new(
            inner.network,
            rust_txs(inner.account.payment_history()),
            inner
                .account
                .header_store()
                .and_then(|headers| headers.tip()),
        );
        for entry in inner.account.tx_history() {
            if let Some(summary) = snapshot
                .history
                .iter_mut()
                .find(|summary| summary.txid == entry.txid.to_string())
            {
                summary.fee = entry.fee.unwrap_or(0);
                summary.fee_known = entry.fee.is_some();
            }
            if let Some(tx) = entry.tx {
                snapshot.transactions.insert(entry.txid, tx);
            }
        }
        for scanner in inner.account.scanners() {
            snapshot.add_scanner(scanner);
        }
        for (outpoint, coin) in inner.account.coins() {
            let output = bitcoin::TxOut {
                value: coin.amount(),
                script_pubkey: coin.script().clone(),
            };
            snapshot.owned_scripts.insert(output.script_pubkey.clone());
            snapshot.prevouts.insert(outpoint, output);
        }
        for (outpoint, _) in inner.account.all_coins() {
            if let Some(output) = snapshot
                .transactions
                .get(&outpoint.txid)
                .and_then(|tx| tx.output.get(outpoint.vout as usize))
            {
                snapshot.owned_scripts.insert(output.script_pubkey.clone());
                snapshot.prevouts.insert(outpoint, output.clone());
            }
        }
        snapshot.update_history_fees();
        Ok(snapshot)
    }

    pub fn update_tx_label(&self, txid: String, label: String) -> crate::ffi::LabelEditResult {
        self.edit_tx_label(&txid, label).into()
    }

    fn edit_tx_label(&self, txid: &str, label: String) -> Result<(), String> {
        let inner = self.inner.as_ref().ok_or("Account not initialized")?;
        let txid = txid.parse::<bitcoin::Txid>().map_err(|e| e.to_string())?;
        let mut result = inner
            .account
            .try_update_tx_label(txid, label.clone())
            .map_err(|e| format!("Label is not saved: {e}. Edit it again to retry."));
        for scanner in inner.account.scanners() {
            let edited = crate::tx_details::edit_scanner_label(
                scanner,
                bwk::bwk_electrum::label_store::LabelKey::Transaction(txid),
                label.clone(),
            );
            result = result.and(edited);
        }
        result
    }

    /// Get the account SP descriptor.
    pub fn descriptor(&self) -> String {
        let Some(inner) = &self.inner else {
            return String::new();
        };
        match crate::config::Config::from_file(inner.name.clone()) {
            Ok(config) => config.get_descriptor(),
            Err(e) => {
                log::error!("failed to load descriptor: {e}");
                String::new()
            }
        }
    }

    /// Descriptor and derivation path to verify `address` on a signing device.
    /// The silent payment address gives the account's sp() descriptor and an empty
    /// path, as it carries no label. Empty when `address` is none of the account's.
    pub fn address_verify_info(&self, address: &str) -> AddressVerifyInfo {
        let empty = AddressVerifyInfo {
            descriptor: String::new(),
            path: String::new(),
        };
        let Some(inner) = &self.inner else {
            return empty;
        };
        if inner.account.sp_address().to_string() == address {
            return AddressVerifyInfo {
                descriptor: self.descriptor(),
                path: "m".to_string(),
            };
        }
        for scanner in inner.account.scanners() {
            for entry in scanner.address_entries() {
                if entry.address.clone().assume_checked().to_string() != address {
                    continue;
                }
                let branch: u32 = match entry.account {
                    KeyChain::Receive => 0,
                    KeyChain::Change => 1,
                    KeyChain::Custom(c) => c,
                };
                return AddressVerifyInfo {
                    descriptor: scanner.descriptor_str(),
                    path: format!("{branch}/{}", entry.index),
                };
            }
        }
        empty
    }

    /// Build a configured TxBuilder from a TransactionTemplate.
    fn build_tx_builder(
        inner: &AccountInner,
        tx_template: &TransactionTemplate,
    ) -> Result<bwk_sp::bwk_tx::tx_builder::TxBuilder, TemplateError> {
        let network = inner.network;
        let mut has_max = false;
        let mut max_addr = None;
        let mut output_total: u64 = 0;
        let mut recipients: Vec<SpRecipientAddress> = Vec::new();

        // Parse outputs
        for output in &tx_template.outputs {
            let addr = RecipientAddress::parse(&output.address, network)
                .map_err(|e| TemplateError::Address(output.address.clone(), format!("{e}")))?;

            if output.max {
                if has_max {
                    return Err(TemplateError::MultipleMax);
                }
                has_max = true;
                max_addr = Some(addr);
            } else {
                output_total += output.amount;
                recipients.push(SpRecipientAddress::new(addr, output.amount, network));
            }
        }

        // Get builder (pre-configured with coin_source, sp_provider, change)
        let feerate_msats_vb = fee_rate_msats(tx_template.fee_rate, tx_template.fee)?;
        let mut builder = inner.account.tx_builder();
        if tx_template.fee > 0 {
            builder = builder.fee(tx_template.fee);
        } else {
            builder = builder.feerate(feerate_msats_vb);
        }

        // Add outputs
        for recip in recipients {
            builder.add_output(recip);
        }
        if has_max {
            let addr = max_addr.unwrap();
            let mut recip = SpRecipientAddress::new(addr, 0, network);
            recip.amount = bwk_sp::bwk_tx::transaction::Amount::Max(None);
            builder.add_output(recip);
        }

        // Handle inputs
        if !tx_template.input_outpoints.is_empty() {
            // Manual coin selection
            let sp_coins = inner.account.coins();
            for outpoint_str in &tx_template.input_outpoints {
                let outpoint = parse_outpoint(outpoint_str)?;
                // Try SP coins first
                if let Some(entry) = sp_coins.get(&outpoint) {
                    if !entry.is_spendable() {
                        return Err(TemplateError::NotSpendable(outpoint_str.clone()));
                    }
                    builder.add_input(sp_coin_entry_to_coin(outpoint, entry));
                } else {
                    // Try sub-account coins
                    let mut found = false;
                    for sub in inner.account.scanners() {
                        let sub_coins = sub.coins();
                        if let Some(entry) = sub_coins.get(&outpoint) {
                            if matches!(
                                entry.status(),
                                bwk_sp::bwk::bwk_coin::CoinStatus::Spent
                                    | bwk_sp::bwk::bwk_coin::CoinStatus::BeingSpend
                            ) {
                                return Err(TemplateError::NotSpendable(outpoint_str.clone()));
                            }
                            builder.add_input(entry.coin.clone());
                            found = true;
                            break;
                        }
                    }
                    if !found {
                        return Err(TemplateError::CoinNotFound(outpoint_str.clone()));
                    }
                }
            }
        } else if has_max {
            // Drain: add all spendable coins
            builder.drain_inputs();
        } else {
            // Auto coin selection
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
                BackendErrorKind::Other,
                "Account not initialized".to_string(),
            );
        };

        let builder = match Self::build_tx_builder(inner, &tx_template) {
            Ok(b) => b,
            Err(e) => return err_sim(BackendErrorKind::InvalidInput, e.to_string()),
        };

        crate::tx_preview::simulation_preview(builder.simulate(), tx_template.fee_rate)
    }

    /// Prepare a transaction for signing.
    pub fn prepare_transaction(&self, tx_template: TransactionTemplate) -> Box<PsbtResult> {
        let err_psbt = |e: String| Box::new(PsbtResult::failed(e));

        let Some(inner) = &self.inner else {
            return Box::new(PsbtResult::failed_kind(
                BackendErrorKind::Other,
                "Account not initialized".to_string(),
            ));
        };

        let mut builder = match Self::build_tx_builder(inner, &tx_template) {
            Ok(b) => b,
            Err(e) => {
                log_failed_prepare(inner, &tx_template, &e.to_string());
                return err_psbt(e.to_string());
            }
        };

        match builder.generate_v2() {
            Ok(psbt) => {
                let bytes_v2 = match psbt.serialize() {
                    Ok(bytes) => bytes,
                    Err(e) => {
                        let error = format!("{e:?}");
                        log_failed_prepare(inner, &tx_template, &error);
                        return err_psbt(error);
                    }
                };
                match psbt.to_bitcoin_psbt_with_empty_sp_outputs() {
                    Ok(psbt_v0) => Box::new(PsbtResult::prepared(
                        psbt_v0.serialize(),
                        bytes_v2,
                        psbt.unsigned_tx()
                            .map(|tx| tx.compute_txid().to_string())
                            .unwrap_or_default(),
                    )),
                    Err(e) => {
                        let error = format!("{e:?}");
                        log_failed_prepare(inner, &tx_template, &error);
                        err_psbt(error)
                    }
                }
            }
            Err(e) => {
                let error = format!("{e:?}");
                log_failed_prepare(inner, &tx_template, &error);
                err_psbt(error)
            }
        }
    }

    /// Sign a prepared transaction.
    pub fn sign_transaction(&self, psbt_result: &PsbtResult) -> TxResult {
        if self.inner.is_none() {
            return TxResult::failed(BackendErrorKind::Other, "Account not initialized".into());
        };

        if let Err(e) = &psbt_result.inner {
            return TxResult::failed(
                BackendErrorKind::InvalidInput,
                format!("Cannot sign invalid transaction: {e}"),
            );
        };

        TxResult::failed(BackendErrorKind::Signing, "External signer required".into())
    }

    /// Broadcast a signed transaction to the network via Electrum.
    ///
    pub fn broadcast_transaction(&self, signed_tx_hex: String) -> TxResult {
        use bitcoin::consensus::encode::deserialize_hex;
        use bitcoin::Transaction;

        let Some(inner) = &self.inner else {
            return TxResult::failed(BackendErrorKind::Other, "Account not initialized".into());
        };

        let tx: Transaction = match deserialize_hex(&signed_tx_hex) {
            Ok(tx) => tx,
            Err(e) => {
                return TxResult::failed(
                    BackendErrorKind::InvalidInput,
                    format!("Invalid transaction hex: {e}"),
                );
            }
        };

        let txid = tx.compute_txid();

        if let Err(e) = broadcast_via_electrum(&inner.electrum_url, inner.verify_certificate, &tx) {
            return TxResult::failed(e.kind, e.message);
        }

        // Reflect the spend locally so spent SP coins drop from spendable at once
        // (sub-account inputs are handled by their own Electrum listeners).
        if let Err(e) = inner.account.record_unconfirmed_spend(&tx) {
            log::warn!("record_unconfirmed_spend after broadcast failed: {e}");
        }

        TxResult::succeeded(txid.to_string())
    }

    /// Finalize a signed PSBT and return transaction hex.
    pub fn finalize_psbt(&self, signed_psbt_base64: String) -> TxResult {
        use base64::{engine::general_purpose::STANDARD, Engine};
        use bitcoin::consensus::encode::serialize_hex;

        let Some(inner) = &self.inner else {
            return TxResult::failed(BackendErrorKind::Other, "Account not initialized".into());
        };

        let psbt = match STANDARD.decode(signed_psbt_base64.trim()) {
            Ok(psbt) => psbt,
            Err(e) => {
                return TxResult::failed(
                    BackendErrorKind::InvalidInput,
                    format!("Invalid PSBT base64: {e}"),
                );
            }
        };

        match inner.account.finalize(&psbt) {
            Ok(tx) => TxResult::succeeded(serialize_hex(&tx)),
            Err(e) => TxResult::failed(
                BackendErrorKind::Signing,
                format!("Failed to finalize PSBT: {e}"),
            ),
        }
    }

    /// Sign and broadcast a transaction in one step.
    pub fn sign_and_broadcast(&self, psbt_result: &PsbtResult) -> TxResult {
        self.sign_transaction(psbt_result)
    }
    /// Validate a prepared transaction via Electrum before signing.
    /// Checks for output address reuse and already-spent inputs.
    pub fn validate_before_sign(&self, psbt_result: &PsbtResult) -> crate::ffi::PsbtValidation {
        let err_validation = |error: String| crate::ffi::PsbtValidation {
            is_ok: false,
            error,
            is_valid: false,
            issues: String::new(),
            reused_output_count: 0,
            spent_input_count: 0,
        };

        let Some(inner) = &self.inner else {
            return err_validation("Account not initialized".to_string());
        };

        psbt_validation(psbt_result, &inner.electrum_url, inner.verify_certificate)
    }

    /// Log all details of a failed broadcast for debugging/reproduction.
    /// Called from C++ as a separate step after broadcast_transaction returns an error.
    pub fn log_failed_broadcast(&self, tx_template: TransactionTemplate, signed_tx_hex: String) {
        use bitcoin::consensus::encode::deserialize_hex;
        use bitcoin::Transaction;

        let Some(inner) = &self.inner else {
            log::error!("log_failed_broadcast: account not initialized");
            return;
        };

        let tx: Transaction = match deserialize_hex(&signed_tx_hex) {
            Ok(tx) => tx,
            Err(e) => {
                log::error!("log_failed_broadcast: cannot decode tx hex: {e}");
                log::error!("signed_tx_hex: {signed_tx_hex}");
                return;
            }
        };

        log_failed_broadcast_impl(inner, &tx, &signed_tx_hex, &tx_template);
    }
}

/// Map bwk payments to the FFI history, newest first: unconfirmed (height 0) on top,
/// then by height descending.
pub(crate) fn rust_txs(payments: Vec<Payment>) -> Vec<RustTx> {
    let mut txs: Vec<RustTx> = payments
        .into_iter()
        .map(|payment| {
            let direction = match payment.payment_type {
                PaymentType::Receive => "incoming",
                PaymentType::Send => "outgoing",
                PaymentType::ToSelf => "self",
            };
            RustTx {
                txid: payment.txid,
                direction: direction.to_string(),
                amount: payment.amount,
                amount_known: true,
                fee: 0,
                fee_known: false,
                height: payment.height.unwrap_or(0) as u32,
                timestamp: payment.timestamp.unwrap_or(0),
                label: payment.label,
                status: match payment.status {
                    bwk::bwk_electrum::coin_store::PaymentStatus::Unconfirmed => "unconfirmed",
                    bwk::bwk_electrum::coin_store::PaymentStatus::ConfirmedUnverified => {
                        "confirmed-unverified"
                    }
                    bwk::bwk_electrum::coin_store::PaymentStatus::Verified => "verified",
                    bwk::bwk_electrum::coin_store::PaymentStatus::VerifyFailed => "verify-failed",
                }
                .into(),
            }
        })
        .collect();

    txs.sort_by(|a, b| match (a.height, b.height) {
        (0, 0) => std::cmp::Ordering::Equal,
        (0, _) => std::cmp::Ordering::Less,
        (_, 0) => std::cmp::Ordering::Greater,
        (x, y) => y.cmp(&x),
    });

    txs
}

/// Validate a prepared transaction via Electrum before signing: output address reuse
/// and already-spent inputs. Blocking.
pub(crate) fn psbt_validation(
    psbt_result: &PsbtResult,
    electrum_url: &str,
    verify_certificate: bool,
) -> crate::ffi::PsbtValidation {
    let err_validation = |error: String| crate::ffi::PsbtValidation {
        is_ok: false,
        error,
        is_valid: false,
        issues: String::new(),
        reused_output_count: 0,
        spent_input_count: 0,
    };

    let bytes_v2 = match psbt_result.v2_bytes() {
        Ok(bytes) => bytes,
        Err(e) => return err_validation(format!("Invalid PSBT: {e}")),
    };

    let psbt = match crate::keys::psbt::parse_v2(bytes_v2) {
        Ok(psbt) => psbt,
        Err(e) => return err_validation(format!("Invalid PSBT: {e}")),
    };

    match validate_psbt_via_electrum(&psbt, electrum_url, verify_certificate) {
        Ok(issue) => {
            let mut issues = Vec::new();
            for (idx, addr) in &issue.reused_outputs {
                issues.push(format!("Output #{idx}: address {addr} already used"));
            }
            for (idx, outpoint, spending_txid) in &issue.spent_inputs {
                issues.push(format!(
                    "Input #{idx}: {outpoint} already spent in {spending_txid}"
                ));
            }
            let is_valid = issue.reused_outputs.is_empty() && issue.spent_inputs.is_empty();
            crate::ffi::PsbtValidation {
                is_ok: true,
                error: String::new(),
                is_valid,
                issues: issues.join("\n"),
                reused_output_count: issue.reused_outputs.len() as u32,
                spent_input_count: issue.spent_inputs.len() as u32,
            }
        }
        Err(e) => err_validation(e),
    }
}

/// Log transaction template and wallet coins on prepare failure for debugging.
fn log_failed_prepare(inner: &AccountInner, tx_template: &TransactionTemplate, error: &str) {
    log::error!("=== PREPARE FAILED ===");
    log::error!("error: {error}");
    log::error!("tx_template: {tx_template:#?}");

    log::error!("sp_coins: {:#?}", inner.account.coins());
    for (sub_idx, sub) in inner.account.scanners().enumerate() {
        let name = match sub_idx {
            0 => "segwit",
            1 => "taproot",
            _ => "unknown_sub",
        };
        log::error!("{name}_coins: {:#?}", sub.coins());
    }

    log::error!("=== END PREPARE FAILED ===");
}

/// Log all transaction details on broadcast failure for debugging.
fn log_failed_broadcast_impl(
    inner: &AccountInner,
    tx: &bitcoin::Transaction,
    signed_tx_hex: &str,
    tx_template: &TransactionTemplate,
) {
    let sp_coins = inner.account.coins();
    let subs: Vec<_> = inner.account.scanners().collect();

    log::error!("=== BROADCAST FAILED ===");

    // Transaction template (original intent)
    log::error!(
        "tx_template: fee_rate={} fee={}",
        tx_template.fee_rate,
        tx_template.fee
    );
    for (i, out) in tx_template.outputs.iter().enumerate() {
        log::error!(
            "  tmpl_output[{i}]: address={} amount={} label={} max={}",
            out.address,
            out.amount,
            out.label,
            out.max
        );
    }
    if !tx_template.input_outpoints.is_empty() {
        log::error!("  tmpl_inputs: {:?}", tx_template.input_outpoints);
    } else {
        log::error!("  tmpl_inputs: auto");
    }

    // Signed transaction details
    log::error!("txid: {}", tx.compute_txid());
    log::error!("version: {}", tx.version);
    log::error!("lock_time: {}", tx.lock_time);

    // Inputs
    log::error!("inputs ({}):", tx.input.len());
    let mut input_total: u64 = 0;
    for (i, input) in tx.input.iter().enumerate() {
        let outpoint = input.previous_output;

        // Determine source and value from wallet coins
        let (source, value) = if let Some(entry) = sp_coins.get(&outpoint) {
            ("sp", entry.amount_sat())
        } else {
            let mut found = ("unknown", 0u64);
            for (sub_idx, sub) in subs.iter().enumerate() {
                if let Some(entry) = sub.coins().get(&outpoint) {
                    let name = match sub_idx {
                        0 => "segwit",
                        1 => "taproot",
                        _ => "unknown_sub",
                    };
                    found = (name, entry.amount_sat());
                    break;
                }
            }
            found
        };
        input_total += value;

        let witness_len = input.witness.len();
        let script_sig_len = input.script_sig.len();
        log::error!(
            "  [{i}] {outpoint} source={source} value={value} \
             sequence={} witness_items={witness_len} script_sig_len={script_sig_len}",
            input.sequence
        );
        // Log witness hex for each item
        for (w, item) in input.witness.iter().enumerate() {
            use std::fmt::Write;
            let item_hex = item.iter().fold(String::new(), |mut s, b| {
                let _ = write!(s, "{b:02x}");
                s
            });
            log::error!("      witness[{w}]: ({} bytes) {item_hex}", item.len());
        }
    }

    // Outputs
    log::error!("outputs ({}):", tx.output.len());
    let mut output_total: u64 = 0;
    for (i, output) in tx.output.iter().enumerate() {
        let value = output.value.to_sat();
        output_total += value;
        log::error!(
            "  [{i}] value={value} script_pubkey={}",
            output.script_pubkey.to_hex_string()
        );
    }

    // Summary
    let fee = input_total.saturating_sub(output_total);
    log::error!("input_total={input_total} output_total={output_total} fee={fee}");

    // Raw hex
    log::error!("signed_tx_hex: {signed_tx_hex}");
    log::error!("=== END BROADCAST FAILED ===");
}

/// Issues found during PSBT validation.
#[derive(Debug, Clone)]
struct PsbtValidationIssue {
    reused_outputs: Vec<(usize, String)>, // (output_index, address_string)
    spent_inputs: Vec<(usize, String, String)>, // (input_index, outpoint, spending_txid)
}

/// Outputs worth checking for address reuse: SP outputs without a derived script
/// are skipped (a fresh ECDH-derived script cannot be reused), and so are
/// OP_RETURN outputs.
fn outputs_to_check(psbt: &bwk_sp::bwk_tx::bwk_psbt::PsbtV2) -> Vec<(usize, bitcoin::ScriptBuf)> {
    psbt.outputs
        .iter()
        .enumerate()
        .filter_map(|(idx, output)| {
            let spk = output.script_pubkey.clone()?;
            if spk.is_op_return() {
                return None;
            }
            Some((idx, spk))
        })
        .collect()
}

/// Inputs to check for a double-spend, paired with the outpoint they consume.
fn inputs_to_check(psbt: &bwk_sp::bwk_tx::bwk_psbt::PsbtV2) -> Vec<(usize, bitcoin::OutPoint)> {
    psbt.inputs
        .iter()
        .enumerate()
        .map(|(idx, input)| (idx, input.previous_output))
        .collect()
}

/// Validate a PSBT via Electrum before signing.
/// Checks for output address reuse and already-spent inputs.
fn validate_psbt_via_electrum(
    psbt: &bwk_sp::bwk_tx::bwk_psbt::PsbtV2,
    electrum_url: &str,
    verify_certificate: bool,
) -> Result<PsbtValidationIssue, String> {
    use std::collections::HashMap;

    if electrum_url.is_empty() {
        return Err("Electrum server address not configured".to_string());
    }

    let (host, port) = crate::config::parse_electrum_url(electrum_url);
    let host = host.ok_or("Invalid electrum URL: missing host")?;
    let port = port.ok_or("Invalid electrum URL: missing port")?;

    let mut client = bwk_sp::bwk::bwk_electrum::client::Client::new(
        &host,
        port,
        crate::config::certificate_check(verify_certificate),
    )
    .map_err(|e| format!("Connection to {electrum_url} failed: {e}"))?;

    let mut reused_outputs = Vec::new();
    let mut spent_inputs = Vec::new();

    // Check outputs: any destination address already used?
    for (idx, spk) in outputs_to_check(psbt) {
        match client.get_coins_tx_at(&spk) {
            Ok(txids) => {
                if !txids.is_empty() {
                    let addr = bitcoin::Address::from_script(&spk, bitcoin::Network::Bitcoin)
                        .map(|a| a.to_string())
                        .unwrap_or_else(|_| spk.to_hex_string());
                    reused_outputs.push((idx, addr));
                }
            }
            Err(e) => {
                log::warn!("validate_psbt: failed to check output #{idx}: {e}");
            }
        }
    }

    // Check inputs: any coin already spent?
    let mut tx_cache: HashMap<bitcoin::Txid, bitcoin::Transaction> = HashMap::new();

    for (idx, outpoint) in inputs_to_check(psbt) {
        let Some(psbt_input) = psbt.inputs.get(idx) else {
            log::warn!("validate_psbt: missing psbt input data for input #{idx}");
            continue;
        };

        // Get the script pubkey for this input from PSBT data.
        let spk = if let Some(ref witness_utxo) = psbt_input.psbt.witness_utxo {
            witness_utxo.script_pubkey.clone()
        } else if let Some(ref non_witness_utxo) = psbt_input.psbt.non_witness_utxo {
            match non_witness_utxo.output.get(outpoint.vout as usize) {
                Some(prev_out) => prev_out.script_pubkey.clone(),
                None => {
                    log::warn!("validate_psbt: malformed prevout for input #{idx}");
                    continue;
                }
            }
        } else {
            // Fetch the previous tx to get the script pubkey.
            match fetch_or_cache_tx(&mut client, &mut tx_cache, outpoint.txid) {
                Ok(tx) => match tx.output.get(outpoint.vout as usize) {
                    Some(prev_out) => prev_out.script_pubkey.clone(),
                    None => {
                        log::warn!("validate_psbt: malformed prevout for input #{idx}");
                        continue;
                    }
                },
                Err(e) => {
                    log::warn!("validate_psbt: failed to fetch prev tx for input #{idx}: {e}");
                    continue;
                }
            }
        };

        // Get history for this script pubkey.
        let txids = match client.get_coins_tx_at(&spk) {
            Ok(txids) => txids,
            Err(e) => {
                log::warn!("validate_psbt: failed to get history for input #{idx}: {e}");
                continue;
            }
        };

        // Check if any tx in the history spends our outpoint.
        for txid in &txids {
            // Skip the funding transaction itself.
            if *txid == outpoint.txid {
                continue;
            }

            let tx = match fetch_or_cache_tx(&mut client, &mut tx_cache, *txid) {
                Ok(tx) => tx,
                Err(e) => {
                    log::warn!("validate_psbt: failed to fetch tx {txid} for input #{idx}: {e}");
                    continue;
                }
            };

            for tx_input in &tx.input {
                if tx_input.previous_output == outpoint {
                    spent_inputs.push((idx, outpoint.to_string(), txid.to_string()));
                    break;
                }
            }
        }
    }

    Ok(PsbtValidationIssue {
        reused_outputs,
        spent_inputs,
    })
}

/// Fetch a transaction, using cache to avoid duplicate requests.
fn fetch_or_cache_tx(
    client: &mut bwk_sp::bwk::bwk_electrum::client::Client,
    cache: &mut std::collections::HashMap<bitcoin::Txid, bitcoin::Transaction>,
    txid: bitcoin::Txid,
) -> Result<bitcoin::Transaction, String> {
    if let Some(tx) = cache.get(&txid) {
        return Ok(tx.clone());
    }
    let tx = client
        .get_tx(txid)
        .map_err(|e| format!("Failed to fetch tx {txid}: {e}"))?;
    cache.insert(txid, tx.clone());
    Ok(tx)
}

/// Broadcast a transaction via Electrum server.
pub(crate) fn broadcast_via_electrum(
    electrum_url: &str,
    verify_certificate: bool,
    tx: &bitcoin::Transaction,
) -> Result<(), BackendFailure> {
    use bitcoin::consensus::encode::serialize_hex;
    use bwk_sp::bwk::bwk_electrum::electrum::{request::Request, response::Response};
    use std::collections::HashMap;

    if electrum_url.is_empty() {
        return Err(BackendFailure::new(
            BackendErrorKind::InvalidInput,
            "Electrum server address not configured",
        ));
    }

    let (host, port, scheme) = bwk_sp::bwk::bwk_electrum::url::parse_electrum_url(electrum_url)
        .map_err(|e| {
            BackendFailure::new(
                BackendErrorKind::InvalidInput,
                format!("Invalid electrum URL '{electrum_url}': {e}"),
            )
        })?;
    let host = host.ok_or_else(|| {
        BackendFailure::new(
            BackendErrorKind::InvalidInput,
            "Invalid electrum URL: missing host",
        )
    })?;
    let port = port.ok_or_else(|| {
        BackendFailure::new(
            BackendErrorKind::InvalidInput,
            "Invalid electrum URL: missing port",
        )
    })?;

    let ssl = matches!(scheme, bwk_sp::bwk::bwk_electrum::url::ElectrumScheme::Ssl);
    let mut client = bwk_sp::bwk::bwk_electrum::raw_client::Client::new_ssl_maybe(&host, port, ssl)
        .certificate_check(crate::config::certificate_check(verify_certificate))
        .map_err(|e| {
            BackendFailure::new(
                BackendErrorKind::InvalidInput,
                format!("Invalid electrum URL '{electrum_url}': {e}"),
            )
        })?;
    client
        .try_connect(Some(Duration::from_secs(10)))
        .map_err(|e| {
            let msg = format!("Connection to {electrum_url} failed:\n{e}");
            log::error!("{msg}");
            BackendFailure::new(BackendErrorKind::Network, msg)
        })?;

    let raw_tx = serialize_hex(tx);
    let request = Request::tx_broadcast(raw_tx);
    client.try_send(&request).map_err(|e| {
        let msg = format!("Failed to send to {electrum_url}:\n{e}");
        log::error!("{msg}");
        BackendFailure::new(BackendErrorKind::Network, msg)
    })?;

    let req_id = request.id;
    let mut index = HashMap::new();
    index.insert(req_id, request);

    let responses = client.recv(&index).map_err(|e| {
        let msg = format!("No response from {electrum_url}:\n{e}");
        log::error!("{msg}");
        BackendFailure::new(BackendErrorKind::Network, msg)
    })?;

    for r in responses {
        match r {
            Response::TxBroadcast(resp) if resp.id == req_id => return Ok(()),
            Response::Error(e) if e.id == req_id => {
                let msg = format!(
                    "Server {electrum_url} rejected transaction:\n{}",
                    e.error.message
                );
                log::error!("{msg}");
                return Err(BackendFailure::new(BackendErrorKind::Broadcast, msg));
            }
            _ => {}
        }
    }

    let msg = format!("Unexpected response from {electrum_url}");
    log::error!("{msg}");
    Err(BackendFailure::new(BackendErrorKind::Electrum, msg))
}

/// Convert unified bwk Notification to Silent FFI Notification.
pub(crate) fn convert_notification(notif: BwkNotification) -> Notification {
    match notif {
        BwkNotification::Sp(sp) => convert_sp_notification(sp),
        // A chain-tip-advance pass mutated tx state, so the coins need a refresh.
        BwkNotification::CoinUpdate | BwkNotification::HeaderStoreUpdated => Notification {
            flag: NotificationFlag::CoinUpdate,
            payload: String::new(),
            current: 0,
            end: 0,
        },
        // The coin store gained or lost a coin: the coins need a refresh.
        BwkNotification::CoinReceived { .. } | BwkNotification::CoinSpent { .. } => Notification {
            flag: NotificationFlag::CoinUpdate,
            payload: String::new(),
            current: 0,
            end: 0,
        },
        BwkNotification::PaymentHistoryUpdated => Notification {
            flag: NotificationFlag::PaymentUpdated,
            payload: String::new(),
            current: 0,
            end: 0,
        },
        // Nothing consumes header validation progress yet; it is carried across so the
        // UI can pick it up without another bridge change.
        BwkNotification::HeaderProgress(event) => Notification {
            flag: NotificationFlag::HeaderProgress,
            payload: format!("{event:?}"),
            current: 0,
            end: 0,
        },
        BwkNotification::ValidationFailed(failure) => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: format!("{failure:?}"),
            current: 0,
            end: 0,
        },
        BwkNotification::Electrum(e) => convert_electrum_notification(e),
        BwkNotification::Signer(e) => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: format!("{e:?}"),
            current: 0,
            end: 0,
        },
        BwkNotification::AddressTipChanged => Notification {
            flag: NotificationFlag::AddressTipChanged,
            payload: String::new(),
            current: 0,
            end: 0,
        },
        // The header store could not be restarted against its endpoint, so the
        // chain it promotes against stops advancing.
        BwkNotification::HeaderStoreRestart => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: "Header store restart failed".to_string(),
            current: 0,
            end: 0,
        },
        // The merkle client ended, so confirmed entries stay unverified until
        // the header store is restarted.
        BwkNotification::MerkleFetchStopped => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: "Merkle proof fetching stopped".to_string(),
            current: 0,
            end: 0,
        },
        BwkNotification::InvalidElectrumConfig => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: "Invalid Electrum configuration".to_string(),
            current: 0,
            end: 0,
        },
        BwkNotification::InvalidLookAhead => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: "Invalid look-ahead value".to_string(),
            current: 0,
            end: 0,
        },
    }
}

/// Convert SP-specific notification to Silent FFI Notification.
fn convert_sp_notification(sp_notif: SpNotification) -> Notification {
    match sp_notif {
        SpNotification::StartingScan => Notification {
            flag: NotificationFlag::StartingScan,
            payload: String::new(),
            current: 0,
            end: 0,
        },
        SpNotification::ScanStarted { start, end } => Notification {
            flag: NotificationFlag::ScanStarted,
            payload: String::new(),
            current: start,
            end,
        },
        SpNotification::FailStartScanning { message } => Notification {
            flag: NotificationFlag::FailStartScanning,
            payload: message,
            current: 0,
            end: 0,
        },
        SpNotification::FailScan { message } => Notification {
            flag: NotificationFlag::FailScan,
            payload: message,
            current: 0,
            end: 0,
        },
        SpNotification::StoppingScan => Notification {
            flag: NotificationFlag::StoppingScan,
            payload: String::new(),
            current: 0,
            end: 0,
        },
        SpNotification::ScanStopped { .. } => Notification {
            flag: NotificationFlag::ScanStopped,
            payload: String::new(),
            current: 0,
            end: 0,
        },
        SpNotification::ScanReceiveProgress { current, end }
        | SpNotification::ScanSpendProgress { current, end } => Notification {
            flag: NotificationFlag::ScanProgress,
            payload: String::new(),
            current,
            end,
        },
        SpNotification::ScanCompleted => Notification {
            flag: NotificationFlag::ScanCompleted,
            payload: String::new(),
            current: 0,
            end: 0,
        },
        SpNotification::NewOutput(outpoint) => Notification {
            flag: NotificationFlag::NewOutput,
            payload: outpoint.to_string(),
            current: 0,
            end: 0,
        },
        SpNotification::OutputSpent(outpoint) => Notification {
            flag: NotificationFlag::OutputSpent,
            payload: outpoint.to_string(),
            current: 0,
            end: 0,
        },
        // A sub-account's coin, as the SP ones above.
        SpNotification::SubAccountCoinReceived { outpoint, .. } => Notification {
            flag: NotificationFlag::NewOutput,
            payload: outpoint.to_string(),
            current: 0,
            end: 0,
        },
        SpNotification::SubAccountCoinSpent { outpoint, .. } => Notification {
            flag: NotificationFlag::OutputSpent,
            payload: outpoint.to_string(),
            current: 0,
            end: 0,
        },
        // Blocks were replaced from `fork_height`: the coins change.
        SpNotification::Reorg { fork_height } => Notification {
            flag: NotificationFlag::CoinUpdate,
            payload: String::new(),
            current: fork_height,
            end: 0,
        },
        // Silent broadcasts through its own Electrum path, so these only arrive when
        // the account broadcasts on its own; both change local state.
        SpNotification::Broadcasted { txid } => Notification {
            flag: NotificationFlag::CoinUpdate,
            payload: txid.to_string(),
            current: 0,
            end: 0,
        },
        SpNotification::FailBroadcast { message } => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: message,
            current: 0,
            end: 0,
        },
        SpNotification::WaitingForBlocks { tip_height } => Notification {
            flag: NotificationFlag::WaitingForBlocks,
            payload: String::new(),
            current: tip_height,
            end: tip_height,
        },
        SpNotification::NewBlocksDetected {
            from_height,
            to_height,
        } => Notification {
            flag: NotificationFlag::NewBlocksDetected,
            payload: String::new(),
            current: from_height,
            end: to_height,
        },
    }
}

/// Convert Electrum listener notification to Silent FFI Notification.
fn convert_electrum_notification(notif: TxListenerNotif) -> Notification {
    match notif {
        TxListenerNotif::Started => Notification {
            flag: NotificationFlag::ElectrumStarted,
            payload: String::new(),
            current: 0,
            end: 0,
        },
        TxListenerNotif::Connected(url) => Notification {
            flag: NotificationFlag::ElectrumConnected,
            payload: url,
            current: 0,
            end: 0,
        },
        TxListenerNotif::Error(e) => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: e.to_string(),
            current: 0,
            end: 0,
        },
        TxListenerNotif::Stopped => Notification {
            flag: NotificationFlag::ElectrumStopped,
            payload: String::new(),
            current: 0,
            end: 0,
        },
        // The connection dropped (a dead socket the keepalive caught, among others):
        // the listener exited, the account reconnects as on any loss.
        TxListenerNotif::Disconnected => Notification {
            flag: NotificationFlag::ElectrumStopped,
            payload: String::new(),
            current: 0,
            end: 0,
        },
    }
}

// CXX FFI functions

/// Open stored wallet state without starting network services.
pub fn new_account_offline(config: &Config) -> Box<Account> {
    match Account::new_inner_connected(config.clone(), false) {
        Ok(inner) => Box::new(Account {
            inner: Some(inner),
            error: String::new(),
            closed_name: None,
            pending_flush_error: String::new(),
        }),
        Err(error) => {
            log::error!("Failed to open account offline: {error}");
            Box::new(Account {
                inner: None,
                error,
                closed_name: Some(config.account_name.clone()),
                pending_flush_error: String::new(),
            })
        }
    }
}

/// Create a new account.
pub fn new_account(account_name: String) -> Box<Account> {
    let config = match crate::config::Config::from_file(account_name) {
        Ok(c) => c,
        Err(e) => {
            let error = format!("failed to load config: {e}");
            log::error!("{error}");
            return Box::new(Account {
                inner: None,
                error,
                closed_name: None,
                pending_flush_error: String::new(),
            });
        }
    };
    match Account::new_inner(config) {
        Ok(inner) => Box::new(Account {
            inner: Some(inner),
            error: String::new(),
            closed_name: None,
            pending_flush_error: String::new(),
        }),
        Err(e) => {
            let error = format!("Failed to create account: {e}");
            log::error!("{error}");
            Box::new(Account {
                inner: None,
                error,
                closed_name: None,
                pending_flush_error: String::new(),
            })
        }
    }
}

/// Notification receiver that owns the mpsc channel.
/// Designed to be used from a dedicated thread with blocking recv().
pub struct NotificationReceiver {
    receiver: mpsc::Receiver<BwkNotification>,
}

impl NotificationReceiver {
    pub(crate) fn new(receiver: mpsc::Receiver<BwkNotification>) -> Self {
        NotificationReceiver { receiver }
    }

    /// Blocking receive — blocks until a notification arrives.
    /// Returns Poll::err when the channel disconnects (sender dropped).
    pub fn recv(&self) -> Box<Poll> {
        match self.receiver.recv() {
            Ok(notif) => {
                let notification = convert_notification(notif);
                Box::new(Poll::ok(notification))
            }
            Err(_) => Box::new(Poll::err("Notification channel disconnected")),
        }
    }
}

/// Poll result wrapper for CXX.
pub struct Poll {
    is_some: bool,
    notification: Notification,
    error: String,
}

impl Poll {
    /// Create a Poll with a notification.
    pub fn ok(notification: Notification) -> Self {
        Poll {
            is_some: true,
            notification,
            error: String::new(),
        }
    }

    /// Create an empty Poll.
    pub fn none() -> Self {
        Poll {
            is_some: false,
            notification: Notification {
                flag: NotificationFlag::ScanStopped,
                payload: String::new(),
                current: 0,
                end: 0,
            },
            error: String::new(),
        }
    }

    /// Create a Poll with an error.
    pub fn err(error: &str) -> Self {
        Poll {
            is_some: false,
            notification: Notification {
                flag: NotificationFlag::FailScan,
                payload: String::new(),
                current: 0,
                end: 0,
            },
            error: error.to_string(),
        }
    }

    /// Check if poll has a notification.
    pub fn is_some(&self) -> bool {
        self.is_some
    }

    /// Get the notification.
    pub fn get_notification(&self) -> Notification {
        self.notification.clone()
    }

    /// Get error message.
    pub fn get_error(&self) -> String {
        self.error.clone()
    }
}

/// PsbtResult wrapper for CXX.
pub struct PsbtResult {
    inner: Result<PreparedPsbt, String>,
    kind: BackendErrorKind,
    pub(crate) saved_record_id: String,
    pub(crate) save_error: String,
}

#[derive(Debug, Clone)]
struct PreparedPsbt {
    /// v0 serialization, with an empty script for every underived SP output.
    bytes: Vec<u8>,
    /// v2 serialization of the same PSBT, which keeps the SP output information.
    bytes_v2: Vec<u8>,
    txid: String,
}

impl PsbtResult {
    pub(crate) fn prepared(bytes: Vec<u8>, bytes_v2: Vec<u8>, txid: String) -> Self {
        PsbtResult {
            inner: Ok(PreparedPsbt {
                bytes,
                bytes_v2,
                txid,
            }),
            saved_record_id: String::new(),
            save_error: String::new(),
            kind: BackendErrorKind::None,
        }
    }

    pub(crate) fn failed(error: String) -> Self {
        Self::failed_kind(BackendErrorKind::InvalidInput, error)
    }

    pub(crate) fn failed_kind(kind: BackendErrorKind, error: String) -> Self {
        PsbtResult {
            inner: Err(error),
            kind,
            saved_record_id: String::new(),
            save_error: String::new(),
        }
    }

    pub fn saved_record_id(&self) -> String {
        self.saved_record_id.clone()
    }

    pub fn save_error(&self) -> String {
        self.save_error.clone()
    }

    /// The v2 serialization, or the error the PSBT failed to prepare with.
    pub(crate) fn v2_bytes(&self) -> Result<&[u8], String> {
        match &self.inner {
            Ok(prepared) => Ok(&prepared.bytes_v2),
            Err(e) => Err(e.clone()),
        }
    }

    /// Check if result is valid.
    pub fn is_ok(&self) -> bool {
        self.inner.is_ok()
    }

    /// Get error message (only valid if !is_ok()).
    pub fn get_psbt_error(&self) -> String {
        match &self.inner {
            Ok(_) => String::new(),
            Err(e) => e.clone(),
        }
    }

    pub fn get_error_kind(&self) -> crate::ffi::BackendErrorKind {
        self.kind
    }

    /// Get transaction ID preview (only valid if is_ok()).
    pub fn get_txid_preview(&self) -> String {
        match &self.inner {
            Ok(psbt) => psbt.txid.clone(),
            Err(_) => String::new(),
        }
    }

    /// Get the serialized PSBT as hex (only valid if is_ok()).
    pub fn bytes_hex(&self) -> String {
        match &self.inner {
            Ok(psbt) => bytes_to_hex(&psbt.bytes),
            Err(_) => String::new(),
        }
    }

    /// Get the serialized PSBT v2 as hex (only valid if is_ok()).
    pub fn bytes_v2_hex(&self) -> String {
        match &self.inner {
            Ok(psbt) => bytes_to_hex(&psbt.bytes_v2),
            Err(_) => String::new(),
        }
    }
}

fn bytes_to_hex(bytes: &[u8]) -> String {
    use std::fmt::Write;

    bytes.iter().fold(String::new(), |mut hex, byte| {
        let _ = write!(hex, "{byte:02x}");
        hex
    })
}

/// Errors from validating a TransactionTemplate before handing it to the builder.
#[derive(Debug, thiserror::Error)]
pub enum TemplateError {
    #[error("Fee rate must be finite and at least 0.001 sat/vB")]
    FeeRate,
    #[error("Invalid address '{0}': {1}")]
    Address(String, String),
    #[error("Only one output can have max=true")]
    MultipleMax,
    #[error("Coin {0} is not spendable")]
    NotSpendable(String),
    #[error("Coin {0} not found in wallet")]
    CoinNotFound(String),
    #[error("Insufficient funds")]
    InsufficientFunds,
    #[error("Invalid outpoint format '{0}', expected 'txid:vout'")]
    OutpointFormat(String),
    #[error("Invalid txid in '{0}': {1}")]
    Txid(String, String),
    #[error("Invalid vout in '{0}': {1}")]
    Vout(String, String),
}

pub(crate) fn fee_rate_msats(rate: f64, absolute_fee: u64) -> Result<u64, TemplateError> {
    if absolute_fee > 0 {
        // Keep the existing coin-selection estimate when the builder uses an absolute fee.
        return Ok(MSATS_PER_SAT);
    }
    if !rate.is_finite() || rate < 0.001 || rate * MSATS_PER_SAT as f64 >= u64::MAX as f64 {
        return Err(TemplateError::FeeRate);
    }
    Ok((rate * MSATS_PER_SAT as f64).round() as u64)
}

/// Parse an outpoint string ("txid:vout") into a bitcoin::OutPoint.
pub(crate) fn parse_outpoint(s: &str) -> Result<bitcoin::OutPoint, TemplateError> {
    let parts: Vec<&str> = s.split(':').collect();
    if parts.len() != 2 {
        return Err(TemplateError::OutpointFormat(s.to_string()));
    }
    let txid = parts[0]
        .parse()
        .map_err(|e| TemplateError::Txid(s.to_string(), format!("{e}")))?;
    let vout = parts[1]
        .parse()
        .map_err(|e| TemplateError::Vout(s.to_string(), format!("{e}")))?;
    Ok(bitcoin::OutPoint { txid, vout })
}

/// Convert an SpCoinEntry to a bwk_sp::bwk::bwk_coin::Coin for use with TxBuilder.
fn sp_coin_entry_to_coin(
    outpoint: bitcoin::OutPoint,
    entry: &bwk_sp::account::coin_store::SpCoinEntry,
) -> bwk_sp::bwk::bwk_coin::Coin {
    const TR_KEYSPEND_SATISFACTION_WEIGHT: u64 = 66;
    bwk_sp::bwk::bwk_coin::Coin {
        txout: bitcoin::TxOut {
            value: entry.amount(),
            script_pubkey: entry.script().clone(),
        },
        outpoint,
        height: entry.height().map(u64::from),
        sequence: bitcoin::Sequence::ENABLE_RBF_NO_LOCKTIME,
        status: bwk_sp::bwk::bwk_coin::CoinStatus::Confirmed,
        label: None,
        satisfaction_size: TR_KEYSPEND_SATISFACTION_WEIGHT,
        spend_info: bwk_sp::bwk::bwk_coin::CoinSpendInfo::Sp {
            tweak: *entry.tweak(),
        },
    }
}

#[cfg(test)]
mod tests {
    use bitcoin::{
        absolute::LockTime, hashes::Hash, opcodes::all::OP_RETURN, script::Builder, transaction,
        OutPoint, ScriptBuf, Sequence, Txid,
    };
    use bwk_sp::{
        bwk::bwk_electrum::notification::SpNotification,
        bwk_tx::bwk_psbt::{Input as PsbtInput, Output as PsbtOutput, PsbtV2},
    };

    use crate::{
        account::{convert_sp_notification, fee_rate_msats, inputs_to_check, outputs_to_check},
        ffi::NotificationFlag,
    };

    #[test]
    fn scanner_progress_crosses_the_bridge_as_numbers() {
        for event in [
            SpNotification::ScanReceiveProgress {
                current: 123,
                end: 456,
            },
            SpNotification::ScanSpendProgress {
                current: 123,
                end: 456,
            },
        ] {
            let notification = convert_sp_notification(event);
            assert_eq!(
                notification.flag,
                NotificationFlag::ScanProgress,
                "progress event kind"
            );
            assert_eq!(
                (notification.current, notification.end),
                (123, 456),
                "numeric progress"
            );
            assert!(
                notification.payload.is_empty(),
                "no comma-separated progress payload"
            );
        }
        let notification =
            convert_sp_notification(SpNotification::WaitingForBlocks { tip_height: 789 });
        assert_eq!(notification.current, 789, "numeric waiting tip");
        assert!(
            notification.payload.is_empty(),
            "no decimal-text waiting tip"
        );
    }

    #[test]
    fn fee_rate_conversion_preserves_sub_one_and_rejects_invalid_rates() {
        for (rate, expected) in [(0.001, 1), (0.125, 125), (0.5, 500), (1.0, 1000)] {
            assert_eq!(
                fee_rate_msats(rate, 0).expect("valid fee rate"),
                expected,
                "convert {rate} sat/vB without clamping"
            );
        }
        for rate in [0.0, -1.0, 0.0001, f64::NAN, f64::INFINITY, f64::MAX] {
            assert!(
                fee_rate_msats(rate, 0).is_err(),
                "reject invalid/unrepresentable rate {rate}"
            );
            assert_eq!(
                fee_rate_msats(rate, 1234).expect("absolute fee"),
                1000,
                "absolute fees do not depend on rate {rate}"
            );
        }
    }

    fn input_at(vout: u32) -> PsbtInput {
        PsbtInput {
            previous_output: OutPoint::new(Txid::from_byte_array([0x22; 32]), vout),
            sequence: Sequence::MAX,
            required_time_lock_time: None,
            required_height_lock_time: None,
            psbt: bitcoin::psbt::Input::default(),
        }
    }

    fn output_with_script(script_pubkey: Option<ScriptBuf>) -> PsbtOutput {
        PsbtOutput {
            amount: bitcoin::Amount::from_sat(1_000),
            script_pubkey,
            psbt: bitcoin::psbt::Output::default(),
        }
    }

    fn psbt_fixture(inputs: Vec<PsbtInput>, outputs: Vec<PsbtOutput>) -> PsbtV2 {
        PsbtV2 {
            tx_version: transaction::Version::TWO,
            fallback_lock_time: Some(LockTime::ZERO),
            tx_modifiable: None,
            xpub: Default::default(),
            proprietary: Default::default(),
            unknown: Default::default(),
            inputs,
            outputs,
        }
    }

    #[test]
    fn outputs_to_check_skips_pending_sp_and_op_return() {
        let scripted = ScriptBuf::new_p2wpkh(&bitcoin::WPubkeyHash::from_byte_array([0x33; 20]));
        let op_return = Builder::new().push_opcode(OP_RETURN).into_script();
        let psbt = psbt_fixture(
            vec![],
            vec![
                output_with_script(Some(scripted.clone())),
                output_with_script(None),
                output_with_script(Some(op_return)),
            ],
        );

        let checked = outputs_to_check(&psbt);

        assert_eq!(checked, vec![(0, scripted)]);
    }

    #[test]
    fn inputs_to_check_returns_every_previous_output() {
        let first = input_at(0);
        let second = input_at(1);
        let expected = [first.previous_output, second.previous_output];
        let psbt = psbt_fixture(vec![first, second], vec![]);

        let checked = inputs_to_check(&psbt);

        assert_eq!(checked, vec![(0, expected[0]), (1, expected[1])]);
    }
}
