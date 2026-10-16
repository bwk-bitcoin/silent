//! MusigAccount: CXX surface over bwk::Account (musig feature). No logic here.

#![allow(dead_code, unused_variables)]

use std::sync::mpsc;

use crate::account::{NotificationReceiver, Poll, PsbtResult};
use crate::ffi::{
    AddressVerifyInfo, CoinState, MusigDescriptorResult, MusigRoundStatus, MusigSpendLeaf,
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
    account: bwk::account::Account,
    receiver: Option<mpsc::Receiver<bwk::bwk_electrum::notification::Notification>>,
    config: crate::config::Config,
}

pub const MUSIG_PLUGIN_ID: &str = "musig";

pub fn new_musig_account(account_name: String) -> Box<MusigAccount> {
    todo!()
}

pub fn validate_musig_descriptor(descriptor: String) -> MusigDescriptorResult {
    todo!()
}

pub fn musig_descriptor_fingerprints(descriptor: String) -> Vec<String> {
    todo!()
}

impl MusigAccount {
    pub fn is_ok(&self) -> bool {
        self.inner.is_some()
    }

    pub fn get_error(&self) -> String {
        self.error.clone()
    }

    pub fn name(&self) -> String {
        todo!()
    }

    pub fn network(&self) -> Network {
        todo!()
    }

    pub fn descriptor(&self) -> String {
        todo!()
    }

    pub fn cosigner_fingerprints(&self) -> Vec<String> {
        todo!()
    }

    pub fn start_electrum(&mut self) -> bool {
        todo!()
    }

    pub fn stop_electrum(&mut self) {
        todo!()
    }

    pub fn electrum_url(&self) -> String {
        todo!()
    }

    pub fn set_electrum_url(&mut self, url: String) -> bool {
        todo!()
    }

    pub fn try_recv(&mut self) -> Box<Poll> {
        todo!()
    }

    pub fn take_receiver(&mut self) -> Box<NotificationReceiver> {
        todo!()
    }

    pub fn balance(&self) -> u64 {
        todo!()
    }

    pub fn new_addr(&mut self) -> String {
        todo!()
    }

    pub fn coins(&self) -> Vec<RustCoin> {
        todo!()
    }

    pub fn spendable_coins(&self) -> CoinState {
        todo!()
    }

    pub fn update_coin_label(&mut self, outpoint: String, label: String) -> bool {
        todo!()
    }

    pub fn payment_history(&self) -> Vec<RustTx> {
        todo!()
    }

    pub fn address_verify_info(&self, address: &str) -> AddressVerifyInfo {
        todo!()
    }

    pub fn simulate_transaction(&self, tx_template: TransactionTemplate) -> TransactionSimulation {
        todo!()
    }

    pub fn spend_leaves(&self, outpoints: Vec<String>) -> MusigSpendLeaves {
        todo!()
    }

    pub fn prepare_transaction(
        &self,
        tx_template: TransactionTemplate,
        spend: MusigSpendLeaf,
    ) -> Box<PsbtResult> {
        todo!()
    }

    pub fn round_status(&self, psbt_v2_hex: &str) -> MusigRoundStatus {
        todo!()
    }

    pub fn finalize_psbt(&self, psbt_v2_hex: &str) -> TxResult {
        todo!()
    }

    pub fn broadcast_transaction(&self, signed_tx_hex: String) -> TxResult {
        todo!()
    }

    pub fn validate_before_sign(&self, psbt_result: &PsbtResult) -> PsbtValidation {
        todo!()
    }
}
