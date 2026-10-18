//! MusigAccount: CXX surface over bwk::Account (musig feature). No logic here.

use std::collections::BTreeSet;
use std::str::FromStr;
use std::sync::mpsc;

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

use crate::account::{
    broadcast_via_electrum, convert_notification, parse_outpoint, psbt_validation, rust_txs,
    NotificationReceiver, Poll, PsbtResult, TemplateError,
};
use crate::config::{parse_electrum_url, Config};
use crate::ffi::{
    AddressVerifyInfo, CoinState, MusigCosignerStatus, MusigDescriptorResult,
    MusigElectrumUrlResult, MusigReceiveAddress, MusigRound, MusigRoundStatus, MusigSpendLeaf,
    MusigSpendLeaves, Network, PsbtValidation, RustCoin, RustTx, TransactionSimulation,
    TransactionTemplate, TxResult,
};

/// A MuSig2 policy account, wrapping bwk::Account.
/// Always constructible; check is_ok() before use.
pub struct MusigAccount {
    inner: Option<MusigAccountInner>,
    error: String,
}

struct MusigAccountInner {
    account: BwkAccount,
    receiver: Option<mpsc::Receiver<BwkNotification>>,
    config: Config,
}

pub const MUSIG_PLUGIN_ID: &str = "musig";

/// The receive index a new account hands out first: bwk moves its tip off 0 before use.
const FIRST_RECEIVE_INDEX: u32 = 1;

const NOT_INITIALIZED: &str = "Account not initialized";

/// bwk's plain tx builder has no silent payment updater to derive such an output.
const SP_RECIPIENT_UNSUPPORTED: &str =
    "silent payment recipients are not supported by this account";

pub fn new_musig_account(account_name: String) -> Box<MusigAccount> {
    let config = match Config::from_file(account_name) {
        Ok(c) => c,
        Err(e) => {
            let error = format!("failed to load config: {e}");
            log::error!("{error}");
            return Box::new(MusigAccount { inner: None, error });
        }
    };
    match MusigAccount::new_inner(config) {
        Ok(inner) => Box::new(MusigAccount {
            inner: Some(inner),
            error: String::new(),
        }),
        Err(e) => {
            let error = format!("Failed to create account: {e}");
            log::error!("{error}");
            Box::new(MusigAccount { inner: None, error })
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
        let musig_config = config.to_musig_config()?;
        let mut account = BwkAccount::try_new(musig_config).map_err(|e| e.to_string())?;
        let receiver = account.receiver();
        Ok(MusigAccountInner {
            account,
            receiver,
            config,
        })
    }

    pub fn is_ok(&self) -> bool {
        self.inner.is_some()
    }

    pub fn get_error(&self) -> String {
        self.error.clone()
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
        inner.account.set_electrum(host, port.to_string());
        inner.config = config;
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

    /// Save the Electrum endpoint to the config file and restart the listener. An empty
    /// or unparsable url is saved but stops it. A failed write keeps the old url and the
    /// running listener, and reports `saved` false.
    pub fn set_electrum_url(&mut self, url: String) -> MusigElectrumUrlResult {
        let Some(inner) = &mut self.inner else {
            return MusigElectrumUrlResult {
                saved: false,
                started: false,
            };
        };
        let previous = inner.config.clone();
        inner.config.set_electrum_url(url);
        if let Err(e) = inner.config.write_file() {
            log::error!("set_electrum_url: {e}");
            // Nothing changed on disk: keep the old url and leave Electrum as it runs.
            inner.config = previous;
            return MusigElectrumUrlResult {
                saved: false,
                started: false,
            };
        }
        self.stop_electrum();
        MusigElectrumUrlResult {
            saved: true,
            started: self.start_electrum(),
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
        inner
            .account
            .scanner()
            .coins()
            .into_values()
            .map(|entry| RustCoin {
                outpoint: entry.outpoint_str(),
                value: entry.amount_sat(),
                height: entry.height().unwrap_or(0) as u32,
                label: entry.label(),
                spent: matches!(entry.status(), CoinStatus::Spent | CoinStatus::BeingSpend),
                // A musig account is a taproot wallet.
                account_type: "Taproot".to_string(),
            })
            .collect()
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

    pub fn update_coin_label(&mut self, outpoint: String, label: String) -> bool {
        let Some(inner) = &mut self.inner else {
            return false;
        };
        if let Err(e) = parse_outpoint(&outpoint) {
            log::error!("update_coin_label: {e}");
            return false;
        }
        inner.account.scanner().update_coin_label(outpoint, label);
        true
    }

    /// Payment history, newest first.
    pub fn payment_history(&self) -> Vec<RustTx> {
        match &self.inner {
            Some(inner) => rust_txs(inner.account.scanner().payment_history()),
            None => vec![],
        }
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

        let feerate_msats_vb = (tx_template.fee_rate.max(1.0) * 1000.0) as u64;
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
            return err_sim(NOT_INITIALIZED.to_string());
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

        let Some(inner) = &self.inner else {
            return failed(NOT_INITIALIZED.to_string());
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
        if let Err(e) = inner.account.restrict_spend_path(&mut psbt, path) {
            return failed(format!("Failed to restrict the spend path: {e}"));
        }

        let bytes_v2 = match PsbtV2::from_bitcoin_psbt(psbt.clone()).and_then(|v2| v2.serialize()) {
            Ok(bytes) => bytes,
            Err(e) => return failed(format!("{e:?}")),
        };
        let txid = psbt.unsigned_tx.compute_txid().to_string();
        Box::new(PsbtResult::prepared(psbt.serialize(), bytes_v2, txid))
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

    /// Aggregate the partial signatures and finalize; the value is the tx hex.
    pub fn finalize_psbt(&self, psbt_v2_hex: &str) -> TxResult {
        let Some(inner) = &self.inner else {
            return tx_error(NOT_INITIALIZED.to_string());
        };
        let mut psbt = match decode_psbt(psbt_v2_hex) {
            Ok(psbt) => psbt,
            Err(e) => return tx_error(e),
        };
        match inner.account.finalize(&mut psbt) {
            Ok(tx) => TxResult {
                is_ok: true,
                error: String::new(),
                value: serialize_hex(&tx),
            },
            Err(e) => tx_error(format!("Failed to finalize PSBT: {e}")),
        }
    }

    /// Broadcast a signed transaction via Electrum; the value is the txid.
    pub fn broadcast_transaction(&self, signed_tx_hex: String) -> TxResult {
        let Some(inner) = &self.inner else {
            return tx_error(NOT_INITIALIZED.to_string());
        };
        let tx: bitcoin::Transaction = match deserialize_hex(&signed_tx_hex) {
            Ok(tx) => tx,
            Err(e) => return tx_error(format!("Invalid transaction hex: {e}")),
        };
        if let Err(e) = broadcast_via_electrum(
            &inner.config.electrum_url,
            inner.config.verify_certificate,
            &tx,
        ) {
            return tx_error(e);
        }
        // Reflect the spend locally so the spent coins drop from spendable at once.
        inner.account.scanner().record_unconfirmed_spend(&tx);
        TxResult {
            is_ok: true,
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
}

/// A recipient as a bitcoin address of `network`; a silent payment address is refused.
fn parse_recipient(
    address: &str,
    network: bitcoin::Network,
) -> Result<bitcoin::Address, TemplateError> {
    if let Ok(RecipientAddress::SpAddress(_)) = RecipientAddress::try_from(address.to_string()) {
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
fn decode_psbt(psbt_v2_hex: &str) -> Result<bitcoin::Psbt, String> {
    let bytes = hex::decode(psbt_v2_hex.trim()).map_err(|e| format!("Invalid PSBT hex: {e}"))?;
    let psbt = crate::keys::psbt::parse_v2(&bytes).map_err(|e| format!("Invalid PSBT: {e}"))?;
    psbt.into_bitcoin_psbt()
        .map_err(|e| format!("Invalid PSBT: {e:?}"))
}

fn tx_error(error: String) -> TxResult {
    TxResult {
        is_ok: false,
        error,
        value: String::new(),
    }
}
