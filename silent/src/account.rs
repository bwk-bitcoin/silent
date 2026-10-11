//! Account module for Silent.
//!
//! Wraps bwk-sp::Account with CXX-compatible interface for C++ bindings.

use std::sync::mpsc;
use std::time::Duration;

use bwk_sp::account::recipient::SpRecipientAddress;
use bwk_sp::account::{Account as SpAccount, ScanMode};
use bwk_sp::bwk::bwk_coin::KeyChain;
use bwk_sp::bwk::bwk_electrum::{
    coin_store::PaymentType,
    notification::{Notification as BwkNotification, SpNotification, TxListenerNotif},
};
use bwk_sp::bwk_sign::miniscript::{Descriptor as MiniscriptDescriptor, DescriptorPublicKey};
use bwk_sp::receiver::RecipientAddress;

use crate::config::Config;
use crate::ffi::{
    AddressVerifyInfo, CoinState, Notification, NotificationFlag, RustCoin, RustTx,
    TransactionSimulation, TransactionTemplate, TxResult,
};

/// Valid account state (created successfully).
struct AccountInner {
    account: SpAccount,
    receiver: Option<mpsc::Receiver<BwkNotification>>,
    network: bitcoin::Network,
    electrum_url: String,
}

/// Account wrapping bwk-sp::Account.
/// Always constructible; check is_ok() before use.
pub struct Account {
    inner: Option<AccountInner>,
    error: String,
}

/// The script of a sub-account, read off its descriptor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SubAccountScript {
    Segwit,
    Taproot,
}

impl SubAccountScript {
    fn of(descriptor: &MiniscriptDescriptor<DescriptorPublicKey>) -> Option<Self> {
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
        })
    }

    /// Create a new AccountInner from config.
    fn new_inner(config: Config) -> Result<AccountInner, String> {
        let network = match config.network {
            crate::ffi::Network::Regtest => bitcoin::Network::Regtest,
            crate::ffi::Network::Signet => bitcoin::Network::Signet,
            crate::ffi::Network::Testnet => bitcoin::Network::Testnet,
            crate::ffi::Network::Bitcoin => bitcoin::Network::Bitcoin,
            _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
        };

        let sp_config = config.to_sp_config()?;
        let mut account = SpAccount::new(sp_config).map_err(|e| e.to_string())?;
        let receiver = account.receiver();

        Ok(AccountInner {
            account,
            receiver,
            network,
            electrum_url: config.electrum_url.clone(),
        })
    }

    /// Check if the account was created successfully.
    pub fn is_ok(&self) -> bool {
        self.inner.is_some()
    }

    /// Get error message (empty if is_ok).
    pub fn get_error(&self) -> String {
        self.error.clone()
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

        let config = match crate::config::Config::from_file(inner.account.name().to_string()) {
            Ok(c) => c,
            Err(e) => {
                log::error!("start_electrum: failed to load config: {e}");
                return false;
            }
        };

        let (host, port) = crate::config::parse_electrum_url(&config.electrum_url);
        inner.account.set_electrum_settings(host, port);
        inner.electrum_url = config.electrum_url;
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
            Some(inner) => inner.account.name().to_string(),
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
        use bwk_sp::account::unified::CoinOrigin;

        let Some(inner) = &self.inner else {
            return vec![];
        };
        let scripts: Vec<Option<SubAccountScript>> = inner
            .account
            .scanners()
            .map(|scanner| SubAccountScript::of(&scanner.descriptor()))
            .collect();
        inner
            .account
            .all_coins()
            .into_values()
            .map(|coin| {
                let account_type = match coin.origin {
                    CoinOrigin::Sp => "SP",
                    CoinOrigin::SubAccount(index) => match scripts.get(index).copied().flatten() {
                        Some(SubAccountScript::Segwit) => "Segwit",
                        Some(SubAccountScript::Taproot) => "Taproot",
                        None => "Unknown",
                    },
                }
                .to_string();
                RustCoin {
                    outpoint: format!("{}:{}", coin.outpoint.txid, coin.outpoint.vout),
                    value: coin.amount.to_sat(),
                    height: coin.height.unwrap_or(0),
                    label: coin.label.unwrap_or_default(),
                    spent: !coin.spendable,
                    account_type,
                }
            })
            .collect()
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
    pub fn update_coin_label(&mut self, outpoint: String, label: String) -> bool {
        let Some(inner) = &mut self.inner else {
            return false;
        };
        // Parse outpoint from "txid:vout" format
        let parts: Vec<&str> = outpoint.split(':').collect();
        if parts.len() != 2 {
            log::error!("Invalid outpoint format, expected 'txid:vout'");
            return false;
        }

        let Ok(txid) = parts[0].parse::<bitcoin::Txid>() else {
            log::error!("Invalid txid: {}", parts[0]);
            return false;
        };
        let Ok(vout) = parts[1].parse::<u32>() else {
            log::error!("Invalid vout: {}", parts[1]);
            return false;
        };

        let outpoint = bitcoin::OutPoint { txid, vout };
        inner.account.update_coin_label(outpoint, label);
        true
    }

    /// Get payment history (SP + sub-accounts), newest first.
    pub fn payment_history(&self) -> Vec<RustTx> {
        let Some(inner) = &self.inner else {
            return vec![];
        };

        // Already aggregated over SP and sub-accounts by bwk.
        let mut txs: Vec<RustTx> = inner
            .account
            .payment_history()
            .into_iter()
            .map(|payment| {
                let direction = match payment.payment_type {
                    PaymentType::Receive => "incoming",
                    PaymentType::Send | PaymentType::ToSelf => "outgoing",
                };
                RustTx {
                    txid: payment.txid,
                    direction: direction.to_string(),
                    amount: payment.amount,
                    fee: 0,
                    height: payment.height.unwrap_or(0) as u32,
                }
            })
            .collect();

        // Newest first: unconfirmed (height 0) on top, then by height descending.
        txs.sort_by(|a, b| match (a.height, b.height) {
            (0, 0) => std::cmp::Ordering::Equal,
            (0, _) => std::cmp::Ordering::Less,
            (_, 0) => std::cmp::Ordering::Greater,
            (x, y) => y.cmp(&x),
        });

        txs
    }

    /// Get the account SP descriptor.
    pub fn descriptor(&self) -> String {
        let Some(inner) = &self.inner else {
            return String::new();
        };
        match crate::config::Config::from_file(inner.account.name().to_string()) {
            Ok(config) => config.get_descriptor(),
            Err(e) => {
                log::error!("failed to load descriptor: {e}");
                String::new()
            }
        }
    }

    /// Descriptor and derivation path to verify `address` on a signing device.
    /// Empty when `address` cannot be resolved to one of the account's segwit or
    /// taproot sub-accounts: the silent payment address itself is not derived at
    /// a BIP32 path, so it has none.
    pub fn address_verify_info(&self, address: &str) -> AddressVerifyInfo {
        let empty = AddressVerifyInfo {
            descriptor: String::new(),
            path: String::new(),
        };
        let Some(inner) = &self.inner else {
            return empty;
        };
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
            let addr = RecipientAddress::try_from(output.address.clone())
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
        let feerate_msats_vb = (tx_template.fee_rate.max(1.0) * 1000.0) as u64;
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
        let err_sim = |error: String| TransactionSimulation {
            is_valid: false,
            fee: 0,
            weight: 0,
            input_total: 0,
            output_total: 0,
            input_count: 0,
            error,
            selected_outpoints: Vec::new(),
        };

        let Some(inner) = &self.inner else {
            return err_sim("Account not initialized".to_string());
        };

        let builder = match Self::build_tx_builder(inner, &tx_template) {
            Ok(b) => b,
            Err(e) => return err_sim(e.to_string()),
        };

        let result = builder.simulate();

        if let Some(ref error) = result.error {
            return err_sim(format!("{error:?}"));
        }

        let input_total: u64 = result
            .tx_template
            .inputs
            .iter()
            .map(|c| c.txout.value.to_sat())
            .sum();
        let fee = result.fees.map(|f| f.to_sat()).unwrap_or(0);
        let output_total = input_total.saturating_sub(fee);
        let input_count = result.tx_template.inputs.len() as u64;

        // Estimate weight from fee and fee_rate
        let weight = if tx_template.fee_rate > 0.0 {
            let vbytes = (fee as f64 / tx_template.fee_rate).ceil() as u64;
            vbytes * 4
        } else {
            0
        };

        let selected_outpoints: Vec<String> = result
            .tx_template
            .inputs
            .iter()
            .map(|c| c.outpoint.to_string())
            .collect();

        log::debug!(
            "simulate_transaction: selected {} outpoints: {:?}",
            selected_outpoints.len(),
            selected_outpoints
        );

        TransactionSimulation {
            is_valid: true,
            fee,
            weight,
            input_total,
            output_total,
            input_count,
            error: String::new(),
            selected_outpoints,
        }
    }

    /// Prepare a transaction for signing.
    pub fn prepare_transaction(&self, tx_template: TransactionTemplate) -> Box<PsbtResult> {
        let err_psbt = |e: String| Box::new(PsbtResult { inner: Err(e) });

        let Some(inner) = &self.inner else {
            return err_psbt("Account not initialized".to_string());
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
                    Ok(psbt_v0) => Box::new(PsbtResult {
                        inner: Ok(PreparedPsbt {
                            bytes: psbt_v0.serialize(),
                            bytes_v2,
                            txid: psbt
                                .unsigned_tx()
                                .map(|tx| tx.compute_txid().to_string())
                                .unwrap_or_default(),
                        }),
                    }),
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
            return TxResult {
                is_ok: false,
                error: "Account not initialized".to_string(),
                value: String::new(),
            };
        };

        if let Err(e) = &psbt_result.inner {
            return TxResult {
                is_ok: false,
                error: format!("Cannot sign invalid transaction: {e}"),
                value: String::new(),
            };
        };

        TxResult {
            is_ok: false,
            error: "External signer required".to_string(),
            value: String::new(),
        }
    }

    /// Broadcast a signed transaction to the network via Electrum.
    ///
    pub fn broadcast_transaction(&self, signed_tx_hex: String) -> TxResult {
        use bitcoin::consensus::encode::deserialize_hex;
        use bitcoin::Transaction;

        let Some(inner) = &self.inner else {
            return TxResult {
                is_ok: false,
                error: "Account not initialized".to_string(),
                value: String::new(),
            };
        };

        let tx: Transaction = match deserialize_hex(&signed_tx_hex) {
            Ok(tx) => tx,
            Err(e) => {
                return TxResult {
                    is_ok: false,
                    error: format!("Invalid transaction hex: {e}"),
                    value: String::new(),
                }
            }
        };

        let txid = tx.compute_txid();

        if let Err(e) = broadcast_via_electrum(&inner.electrum_url, &tx) {
            return TxResult {
                is_ok: false,
                error: e,
                value: String::new(),
            };
        }

        // Reflect the spend locally so spent SP coins drop from spendable at once
        // (sub-account inputs are handled by their own Electrum listeners).
        if let Err(e) = inner.account.record_unconfirmed_spend(&tx) {
            log::warn!("record_unconfirmed_spend after broadcast failed: {e}");
        }

        TxResult {
            is_ok: true,
            error: String::new(),
            value: txid.to_string(),
        }
    }

    /// Finalize a signed PSBT and return transaction hex.
    pub fn finalize_psbt(&self, signed_psbt_base64: String) -> TxResult {
        use base64::{engine::general_purpose::STANDARD, Engine};
        use bitcoin::consensus::encode::serialize_hex;

        let Some(inner) = &self.inner else {
            return TxResult {
                is_ok: false,
                error: "Account not initialized".to_string(),
                value: String::new(),
            };
        };

        let psbt = match STANDARD.decode(signed_psbt_base64.trim()) {
            Ok(psbt) => psbt,
            Err(e) => {
                return TxResult {
                    is_ok: false,
                    error: format!("Invalid PSBT base64: {e}"),
                    value: String::new(),
                }
            }
        };

        match inner.account.finalize(&psbt) {
            Ok(tx) => TxResult {
                is_ok: true,
                error: String::new(),
                value: serialize_hex(&tx),
            },
            Err(e) => TxResult {
                is_ok: false,
                error: format!("Failed to finalize PSBT: {e}"),
                value: String::new(),
            },
        }
    }

    /// Sign and broadcast a transaction in one step.
    pub fn sign_and_broadcast(&self, psbt_result: &PsbtResult) -> TxResult {
        if self.inner.is_none() {
            return TxResult {
                is_ok: false,
                error: "Account not initialized".to_string(),
                value: String::new(),
            };
        };

        if let Err(e) = &psbt_result.inner {
            return TxResult {
                is_ok: false,
                error: format!("Cannot sign invalid transaction: {e}"),
                value: String::new(),
            };
        };

        TxResult {
            is_ok: false,
            error: "External signer required".to_string(),
            value: String::new(),
        }
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

        let prepared = match &psbt_result.inner {
            Ok(prepared) => prepared,
            Err(e) => return err_validation(format!("Invalid PSBT: {e}")),
        };

        let psbt = match crate::keys::psbt::parse_v2(&prepared.bytes_v2) {
            Ok(psbt) => psbt,
            Err(e) => return err_validation(format!("Invalid PSBT: {e}")),
        };

        match validate_psbt_via_electrum(&psbt, &inner.electrum_url) {
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
) -> Result<PsbtValidationIssue, String> {
    use std::collections::HashMap;

    if electrum_url.is_empty() {
        return Err("Electrum server address not configured".to_string());
    }

    let (host, port) = crate::config::parse_electrum_url(electrum_url);
    let host = host.ok_or("Invalid electrum URL: missing host")?;
    let port = port.ok_or("Invalid electrum URL: missing port")?;

    // AccountInner does not carry the account's configured CertificateCheck policy,
    // so this always validates the certificate chain. Accounts pointed at a
    // self-signed or onion electrum server (DangerAcceptInvalid) will not be able
    // to run this check until that policy is threaded through from the config.
    let mut client = bwk_sp::bwk::bwk_electrum::client::Client::new(
        &host,
        port,
        bwk_sp::bwk::bwk_electrum::raw_client::CertificateCheck::Validate,
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
fn broadcast_via_electrum(electrum_url: &str, tx: &bitcoin::Transaction) -> Result<(), String> {
    use bitcoin::consensus::encode::serialize_hex;
    use bwk_sp::bwk::bwk_electrum::electrum::{request::Request, response::Response};
    use std::collections::HashMap;

    if electrum_url.is_empty() {
        return Err("Electrum server address not configured".to_string());
    }

    let (host, port, scheme) = bwk_sp::bwk::bwk_electrum::url::parse_electrum_url(electrum_url)
        .map_err(|e| format!("Invalid electrum URL '{electrum_url}': {e}"))?;
    let host = host.ok_or("Invalid electrum URL: missing host")?;
    let port = port.ok_or("Invalid electrum URL: missing port")?;

    let ssl = matches!(scheme, bwk_sp::bwk::bwk_electrum::url::ElectrumScheme::Ssl);
    let mut client = bwk_sp::bwk::bwk_electrum::raw_client::Client::new_ssl_maybe(&host, port, ssl);
    client
        .try_connect(Some(Duration::from_secs(10)))
        .map_err(|e| {
            let msg = format!("Connection to {electrum_url} failed:\n{e}");
            log::error!("{msg}");
            msg
        })?;

    let raw_tx = serialize_hex(tx);
    let request = Request::tx_broadcast(raw_tx);
    client.try_send(&request).map_err(|e| {
        let msg = format!("Failed to send to {electrum_url}:\n{e}");
        log::error!("{msg}");
        msg
    })?;

    let req_id = request.id;
    let mut index = HashMap::new();
    index.insert(req_id, request);

    let responses = client.recv(&index).map_err(|e| {
        let msg = format!("No response from {electrum_url}:\n{e}");
        log::error!("{msg}");
        msg
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
                return Err(msg);
            }
            _ => {}
        }
    }

    let msg = format!("Unexpected response from {electrum_url}");
    log::error!("{msg}");
    Err(msg)
}

/// Convert unified bwk Notification to Silent FFI Notification.
fn convert_notification(notif: BwkNotification) -> Notification {
    match notif {
        BwkNotification::Sp(sp) => convert_sp_notification(sp),
        // A chain-tip-advance pass mutated tx state, so the coins need a refresh.
        BwkNotification::CoinUpdate | BwkNotification::HeaderStoreUpdated => Notification {
            flag: NotificationFlag::CoinUpdate,
            payload: String::new(),
        },
        BwkNotification::PaymentHistoryUpdated => Notification {
            flag: NotificationFlag::PaymentUpdated,
            payload: String::new(),
        },
        // Nothing consumes header validation progress yet; it is carried across so the
        // UI can pick it up without another bridge change.
        BwkNotification::HeaderProgress(event) => Notification {
            flag: NotificationFlag::HeaderProgress,
            payload: format!("{event:?}"),
        },
        BwkNotification::ValidationFailed(failure) => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: format!("{failure:?}"),
        },
        BwkNotification::Electrum(e) => convert_electrum_notification(e),
        BwkNotification::Signer(e) => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: format!("{e:?}"),
        },
        BwkNotification::AddressTipChanged => Notification {
            flag: NotificationFlag::AddressTipChanged,
            payload: String::new(),
        },
        // The header store could not be restarted against its endpoint, so the
        // chain it promotes against stops advancing.
        BwkNotification::HeaderStoreRestart => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: "Header store restart failed".to_string(),
        },
        // The merkle client ended, so confirmed entries stay unverified until
        // the header store is restarted.
        BwkNotification::MerkleFetchStopped => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: "Merkle proof fetching stopped".to_string(),
        },
        BwkNotification::InvalidElectrumConfig => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: "Invalid Electrum configuration".to_string(),
        },
        BwkNotification::InvalidLookAhead => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: "Invalid look-ahead value".to_string(),
        },
    }
}

/// Convert SP-specific notification to Silent FFI Notification.
fn convert_sp_notification(sp_notif: SpNotification) -> Notification {
    match sp_notif {
        SpNotification::StartingScan => Notification {
            flag: NotificationFlag::StartingScan,
            payload: String::new(),
        },
        SpNotification::ScanStarted { start, end } => Notification {
            flag: NotificationFlag::ScanStarted,
            payload: format!("{start},{end}"),
        },
        SpNotification::FailStartScanning { message } => Notification {
            flag: NotificationFlag::FailStartScanning,
            payload: message,
        },
        SpNotification::FailScan { message } => Notification {
            flag: NotificationFlag::FailScan,
            payload: message,
        },
        SpNotification::StoppingScan => Notification {
            flag: NotificationFlag::StoppingScan,
            payload: String::new(),
        },
        SpNotification::ScanStopped => Notification {
            flag: NotificationFlag::ScanStopped,
            payload: String::new(),
        },
        SpNotification::ScanReceiveProgress { current, end }
        | SpNotification::ScanSpendProgress { current, end } => Notification {
            flag: NotificationFlag::ScanProgress,
            payload: format!("{current},{end}"),
        },
        SpNotification::ScanCompleted => Notification {
            flag: NotificationFlag::ScanCompleted,
            payload: String::new(),
        },
        SpNotification::NewOutput(outpoint) => Notification {
            flag: NotificationFlag::NewOutput,
            payload: outpoint.to_string(),
        },
        SpNotification::OutputSpent(outpoint) => Notification {
            flag: NotificationFlag::OutputSpent,
            payload: outpoint.to_string(),
        },
        // Silent broadcasts through its own Electrum path, so these only arrive when
        // the account broadcasts on its own; both change local state.
        SpNotification::Broadcasted { txid } => Notification {
            flag: NotificationFlag::CoinUpdate,
            payload: txid.to_string(),
        },
        SpNotification::FailBroadcast { message } => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: message,
        },
        SpNotification::WaitingForBlocks { tip_height } => Notification {
            flag: NotificationFlag::WaitingForBlocks,
            payload: tip_height.to_string(),
        },
        SpNotification::NewBlocksDetected {
            from_height,
            to_height,
        } => Notification {
            flag: NotificationFlag::NewBlocksDetected,
            payload: format!("{from_height},{to_height}"),
        },
    }
}

/// Convert Electrum listener notification to Silent FFI Notification.
fn convert_electrum_notification(notif: TxListenerNotif) -> Notification {
    match notif {
        TxListenerNotif::Started => Notification {
            flag: NotificationFlag::ElectrumStarted,
            payload: String::new(),
        },
        TxListenerNotif::Connected(url) => Notification {
            flag: NotificationFlag::ElectrumConnected,
            payload: url,
        },
        TxListenerNotif::Error(e) => Notification {
            flag: NotificationFlag::ElectrumError,
            payload: e.to_string(),
        },
        TxListenerNotif::Stopped => Notification {
            flag: NotificationFlag::ElectrumStopped,
            payload: String::new(),
        },
    }
}

// CXX FFI functions

/// Create a new account.
pub fn new_account(account_name: String) -> Box<Account> {
    let config = match crate::config::Config::from_file(account_name) {
        Ok(c) => c,
        Err(e) => {
            let error = format!("failed to load config: {e}");
            log::error!("{error}");
            return Box::new(Account { inner: None, error });
        }
    };
    match Account::new_inner(config) {
        Ok(inner) => Box::new(Account {
            inner: Some(inner),
            error: String::new(),
        }),
        Err(e) => {
            let error = format!("Failed to create account: {e}");
            log::error!("{error}");
            Box::new(Account { inner: None, error })
        }
    }
}

/// Notification receiver that owns the mpsc channel.
/// Designed to be used from a dedicated thread with blocking recv().
pub struct NotificationReceiver {
    receiver: mpsc::Receiver<BwkNotification>,
}

impl NotificationReceiver {
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

/// Parse an outpoint string ("txid:vout") into a bitcoin::OutPoint.
fn parse_outpoint(s: &str) -> Result<bitcoin::OutPoint, TemplateError> {
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
        height: Some(entry.height() as u64),
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
    use bwk_sp::bwk_tx::bwk_psbt::{Input as PsbtInput, Output as PsbtOutput, PsbtV2};

    use crate::account::{inputs_to_check, outputs_to_check};

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
