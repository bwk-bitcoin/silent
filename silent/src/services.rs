//! Wallet rules behind the account UI services, shared by the SP and Multikey accounts:
//! crafting without implicit selection, the history check of the published read model,
//! labels, broadcast outcomes and endpoint settings.

use std::{collections::BTreeSet, str::FromStr, sync::Mutex};

use bitcoin::{address::NetworkUnchecked, consensus::encode::deserialize, OutPoint, Txid};
use bwk::bwk_electrum::{
    label_store::{LabelKey, LabelStore},
    scanner::ElectrumScanner,
};

use crate::{
    config::Config,
    error::BackendFailure,
    ffi::{
        BackendErrorKind, BackendServer, BroadcastOutcome, BroadcastReport, CoinState,
        CraftOutcome, EndpointResult, EndpointSetting, FundsSnapshot, LabelEditResult, LabelEntry,
        LabelKind, LabelSnapshot, Network, ReceiveAddress, ReceiveAddressResult, RustCoin, RustTx,
        TransactionSimulation, TransactionTemplate, TxResult, UnsignedPsbt,
    },
    musig::MUSIG_PLUGIN_ID,
};

/// Crafting and finalizing never pick coins: selection is its own explicit request.
pub(crate) const NO_INPUTS: &str = "No inputs are selected: select coins first";

/// Receive kind ids.
pub(crate) const SP_KIND: &str = "sp";
pub(crate) const SEGWIT_KIND: &str = "segwit";
pub(crate) const TAPROOT_KIND: &str = "taproot";

/// The receive index a scanner hands out first: bwk moves its tip off 0 before use.
pub(crate) const FIRST_RECEIVE_INDEX: u32 = 1;

/// The template checked against its own inputs: none given leaves it incomplete.
pub(crate) fn craft(
    template: TransactionTemplate,
    simulate: impl FnOnce(TransactionTemplate) -> TransactionSimulation,
) -> CraftOutcome {
    if template.input_outpoints.is_empty() {
        return incomplete(template, NO_INPUTS.into());
    }
    let simulation = simulate(template.clone());
    resolve(template, &simulation)
}

/// The template with the inputs coin selection picks in place of its own.
pub(crate) fn select(
    template: TransactionTemplate,
    simulate: impl FnOnce(TransactionTemplate) -> TransactionSimulation,
) -> CraftOutcome {
    let mut selection = template.clone();
    selection.input_outpoints.clear();
    let simulation = simulate(selection.clone());
    if !simulation.is_valid {
        return incomplete(template, simulation.error);
    }
    selection.input_outpoints = simulation.selected_outpoints.clone();
    resolve(selection, &simulation)
}

/// Fills the MAX output with what the simulation sends past the fixed outputs.
fn resolve(mut template: TransactionTemplate, simulation: &TransactionSimulation) -> CraftOutcome {
    if !simulation.is_valid {
        return incomplete(template, simulation.error.clone());
    }
    let fixed = template
        .outputs
        .iter()
        .filter(|output| !output.max)
        .fold(0_u64, |total, output| total.saturating_add(output.amount));
    if let Some(max) = template.outputs.iter_mut().find(|output| output.max) {
        max.amount = simulation.sending_total.saturating_sub(fixed);
    }
    CraftOutcome {
        complete: true,
        error: String::new(),
        transaction: template,
    }
}

fn incomplete(template: TransactionTemplate, error: String) -> CraftOutcome {
    CraftOutcome {
        complete: false,
        error,
        transaction: template,
    }
}

/// Refuses a template without inputs before any PSBT is built.
pub(crate) fn finalizable(template: &TransactionTemplate) -> Result<(), String> {
    if template.input_outpoints.is_empty() {
        return Err(NO_INPUTS.into());
    }
    Ok(())
}

impl From<Result<Vec<u8>, String>> for UnsignedPsbt {
    fn from(result: Result<Vec<u8>, String>) -> Self {
        match result {
            Ok(psbt) => Self {
                is_ok: true,
                error: String::new(),
                psbt,
            },
            Err(error) => Self {
                is_ok: false,
                error,
                psbt: Vec::new(),
            },
        }
    }
}

pub(crate) fn funds(
    coins: Vec<RustCoin>,
    spendable: Vec<String>,
    balance: CoinState,
    history: Vec<RustTx>,
) -> FundsSnapshot {
    FundsSnapshot {
        is_ok: true,
        error: String::new(),
        history_complete: history_covers(&coins, &history),
        coins,
        spendable,
        balance,
        history,
    }
}

pub(crate) fn funds_failed(error: String) -> FundsSnapshot {
    FundsSnapshot {
        is_ok: false,
        error,
        coins: Vec::new(),
        spendable: Vec::new(),
        balance: CoinState {
            confirmed_count: 0,
            confirmed_balance: 0,
            unconfirmed_count: 0,
            unconfirmed_balance: 0,
        },
        history: Vec::new(),
        history_complete: false,
    }
}

pub(crate) fn address_failed(error: String) -> ReceiveAddressResult {
    ReceiveAddressResult {
        is_ok: false,
        error,
        address: ReceiveAddress {
            kind: String::new(),
            address: String::new(),
            descriptor: String::new(),
            path: String::new(),
        },
    }
}

/// Hands out the scanner's next receive address through its locked coin store, as
/// `ElectrumScanner::new_recv_addr` does without needing the scanner mutably.
pub(crate) fn next_receive_address(scanner: &ElectrumScanner) -> Result<String, String> {
    let mut coins = scanner.coin_store().lock().map_err(|e| e.to_string())?;
    Ok(coins.new_recv_addr().to_string())
}

/// Whether the funding transaction of every coin is in `history`. A store may publish
/// a coin before it writes the transaction that funds it.
pub(crate) fn history_covers(coins: &[RustCoin], history: &[RustTx]) -> bool {
    let txids: BTreeSet<&str> = history.iter().map(|tx| tx.txid.as_str()).collect();
    coins.iter().all(|coin| {
        coin.outpoint
            .rsplit_once(':')
            .is_some_and(|(txid, _)| txids.contains(txid))
    })
}

/// The label `kind` keys on, read from its UI form.
pub(crate) fn label_key(
    kind: LabelKind,
    id: &str,
    network: bitcoin::Network,
) -> Result<LabelKey, String> {
    match kind {
        LabelKind::Address => {
            let address = bitcoin::Address::<NetworkUnchecked>::from_str(id)
                .map_err(|e| format!("Invalid address '{id}': {e}"))?;
            let address = address
                .require_network(network)
                .map_err(|e| format!("Invalid address '{id}': {e}"))?;
            Ok(LabelKey::Address(address.as_unchecked().clone()))
        }
        LabelKind::Outpoint => OutPoint::from_str(id)
            .map(LabelKey::OutPoint)
            .map_err(|e| format!("Invalid outpoint '{id}': {e}")),
        LabelKind::Transaction => Txid::from_str(id)
            .map(LabelKey::Transaction)
            .map_err(|e| format!("Invalid txid '{id}': {e}")),
        _ => Err("Unknown label kind".into()),
    }
}

/// A refused edit: nothing changed.
pub(crate) fn label_failed(error: String) -> LabelEditResult {
    LabelEditResult {
        is_ok: false,
        error,
        changed: false,
    }
}

pub(crate) fn labels_failed(error: String) -> LabelSnapshot {
    LabelSnapshot {
        is_ok: false,
        error,
        labels: Vec::new(),
    }
}

/// Runs `edit` on `key` and reports whether the label `store` holds in RAM changed,
/// saved or not.
pub(crate) fn label_edit(
    store: &Mutex<LabelStore>,
    key: &LabelKey,
    edit: impl FnOnce() -> Result<(), String>,
) -> LabelEditResult {
    let current = || store.lock().ok().and_then(|labels| labels.get(key));
    let before = current();
    let result = edit();
    let changed = current() != before;
    match result {
        Ok(()) => LabelEditResult {
            is_ok: true,
            error: String::new(),
            changed,
        },
        Err(error) => LabelEditResult {
            is_ok: false,
            error,
            changed,
        },
    }
}

/// Edits then saves one label of `store`. An empty `label` removes it; a failed save
/// keeps the RAM edit.
pub(crate) fn edit_store_label(
    store: &Mutex<LabelStore>,
    key: LabelKey,
    label: String,
) -> Result<(), String> {
    let mut labels = store.lock().map_err(|e| e.to_string())?;
    labels
        .try_edit(key, (!label.is_empty()).then_some(label))
        .map_err(|e| e.to_string())?;
    labels
        .try_persist()
        .map_err(|e| format!("Label is not saved: {e}. Edit it again to retry."))
}

/// Every label of `stores`, the first store holding a key winning.
pub(crate) fn label_snapshot(stores: &[&Mutex<LabelStore>]) -> LabelSnapshot {
    let mut keys = BTreeSet::new();
    let mut labels = Vec::new();
    for store in stores {
        let entries = match store.lock() {
            Ok(store) => store.snapshot().map_err(|e| e.to_string()),
            Err(e) => Err(e.to_string()),
        };
        let entries = match entries {
            Ok(entries) => entries,
            Err(error) => return labels_failed(format!("Labels cannot be read: {error}")),
        };
        for (key, text) in entries {
            let entry = label_entry(key, text);
            if keys.insert((entry.kind.repr, entry.id.clone())) {
                labels.push(entry);
            }
        }
    }
    LabelSnapshot {
        is_ok: true,
        error: String::new(),
        labels,
    }
}

fn label_entry(key: LabelKey, text: String) -> LabelEntry {
    let (kind, id) = match key {
        LabelKey::Address(address) => (LabelKind::Address, address.assume_checked().to_string()),
        LabelKey::OutPoint(outpoint) => (LabelKind::Outpoint, outpoint.to_string()),
        LabelKey::Transaction(txid) => (LabelKind::Transaction, txid.to_string()),
    };
    LabelEntry { kind, id, text }
}

/// Why a transaction was not accepted, with what the sender knows of its fate.
pub(crate) struct Undelivered {
    pub outcome: BroadcastOutcome,
    pub failure: BackendFailure,
}

impl Undelivered {
    /// It never reached a server.
    pub(crate) fn not_sent(kind: BackendErrorKind, message: impl Into<String>) -> Self {
        Self {
            outcome: BroadcastOutcome::Rejected,
            failure: BackendFailure::new(
                kind,
                format!("The transaction was not sent: {}", message.into()),
            ),
        }
    }

    pub(crate) fn rejected(message: impl Into<String>) -> Self {
        Self {
            outcome: BroadcastOutcome::Rejected,
            failure: BackendFailure::new(BackendErrorKind::Broadcast, message),
        }
    }

    /// Sent: the server may have it.
    pub(crate) fn unknown(kind: BackendErrorKind, message: impl Into<String>) -> Self {
        Self {
            outcome: BroadcastOutcome::Unknown,
            failure: BackendFailure::new(kind, message),
        }
    }
}

/// Whether a server refusing a transaction says it already holds it: the network has it.
pub(crate) fn already_known(message: &str) -> bool {
    let message = message.to_lowercase();
    [
        "already in block chain",
        "already in blockchain",
        "already in mempool",
        "txn-already-in-mempool",
        "txn-already-known",
    ]
    .iter()
    .any(|known| message.contains(known))
}

/// The transaction of consensus-encoded `transaction`, refused before any send.
pub(crate) fn decode_transaction(transaction: &[u8]) -> Result<bitcoin::Transaction, Undelivered> {
    deserialize(transaction).map_err(|e| {
        Undelivered::not_sent(
            BackendErrorKind::InvalidInput,
            format!("Invalid transaction: {e}"),
        )
    })
}

impl BroadcastReport {
    pub(crate) fn accepted(txid: String, warnings: Vec<String>) -> Self {
        Self {
            outcome: BroadcastOutcome::Accepted,
            txid,
            kind: BackendErrorKind::None,
            error: String::new(),
            warnings,
        }
    }

    pub(crate) fn undelivered(txid: String, undelivered: Undelivered) -> Self {
        Self {
            outcome: undelivered.outcome,
            txid,
            kind: undelivered.failure.kind,
            error: undelivered.failure.message,
            warnings: Vec::new(),
        }
    }

    /// The TxResult of the older broadcast call: the txid once accepted, the warnings
    /// logged.
    pub(crate) fn tx_result(self) -> TxResult {
        for warning in &self.warnings {
            log::warn!("broadcast {}: {warning}", self.txid);
        }
        match self.outcome {
            BroadcastOutcome::Accepted => TxResult::succeeded(self.txid),
            _ => TxResult::failed(self.kind, self.error),
        }
    }
}

/// The URL `server` answers on, tested against `network`.
pub(crate) fn tested_url(
    server: BackendServer,
    url: String,
    verify_certificate: bool,
    network: Network,
) -> Result<String, (BackendErrorKind, String)> {
    match server {
        BackendServer::Electrum => {
            let tested = crate::test_electrum(url, verify_certificate);
            if !tested.is_ok {
                return Err((tested.kind, tested.error));
            }
            Ok(tested.url)
        }
        BackendServer::Blindbit => {
            let tested = crate::get_backend_info(url);
            if !tested.is_ok {
                return Err((tested.kind, tested.error));
            }
            if tested.network != network {
                return Err((
                    BackendErrorKind::InvalidInput,
                    "Backend network does not match the account".into(),
                ));
            }
            Ok(tested.url)
        }
        _ => Err((BackendErrorKind::InvalidInput, "Unknown endpoint".into())),
    }
}

pub fn test_endpoint(
    server: BackendServer,
    url: String,
    verify_certificate: bool,
    network: Network,
) -> EndpointResult {
    match tested_url(server, url, verify_certificate, network) {
        Ok(url) => EndpointResult {
            kind: BackendErrorKind::None,
            connection_pending: false,
            is_ok: true,
            saved: false,
            error: String::new(),
            url,
            verify_certificate,
        },
        Err((kind, error)) => EndpointResult {
            kind,
            error,
            ..EndpointResult::default()
        },
    }
}

pub fn save_endpoints(account_name: String, endpoints: Vec<EndpointSetting>) -> EndpointResult {
    match write_endpoints(account_name, &endpoints) {
        Ok(verify_certificate) => EndpointResult {
            kind: BackendErrorKind::None,
            connection_pending: false,
            is_ok: true,
            saved: true,
            error: String::new(),
            url: String::new(),
            verify_certificate,
        },
        Err((kind, error)) => EndpointResult {
            kind,
            error,
            ..EndpointResult::default()
        },
    }
}

/// Applies every endpoint to the config, then writes it once. Answers the saved
/// certificate check.
fn write_endpoints(
    account_name: String,
    endpoints: &[EndpointSetting],
) -> Result<bool, (BackendErrorKind, String)> {
    let invalid = |error: String| (BackendErrorKind::InvalidInput, error);
    let mut config =
        Config::from_file(account_name).map_err(|e| (BackendErrorKind::Storage, e.to_string()))?;
    let mut seen = BTreeSet::new();
    for endpoint in endpoints {
        if !seen.insert(endpoint.server.repr) {
            return Err(invalid("An endpoint is listed twice".into()));
        }
        let url = endpoint.url.trim().to_string();
        if url.is_empty() {
            return Err(invalid("An endpoint has no address".into()));
        }
        match endpoint.server {
            BackendServer::Electrum => {
                crate::electrum_candidates(&url).map_err(invalid)?;
                config.set_electrum_url(url);
                config.set_verify_certificate(endpoint.verify_certificate);
            }
            BackendServer::Blindbit if config.plugin_id != MUSIG_PLUGIN_ID => {
                config.set_blindbit_url(url);
            }
            BackendServer::Blindbit => {
                return Err(invalid("This account has no BlindBit endpoint".into()))
            }
            _ => return Err(invalid("Unknown endpoint".into())),
        }
    }
    config
        .write_file()
        .map_err(|e| (BackendErrorKind::Storage, e))?;
    Ok(config.verify_certificate)
}

#[cfg(test)]
mod tests {
    use crate::{
        ffi::{RustCoin, RustTx},
        services::history_covers,
    };

    fn coin(outpoint: &str) -> RustCoin {
        RustCoin {
            outpoint: outpoint.into(),
            value: 1000,
            height: 0,
            timestamp: 0,
            label: String::new(),
            spent: false,
            account_type: "SP".into(),
        }
    }

    fn tx(txid: &str) -> RustTx {
        RustTx {
            txid: txid.into(),
            ..RustTx::default()
        }
    }

    #[test]
    fn early_coin_leaves_history_incomplete_until_its_transaction_is_written() {
        let funding = "aa".repeat(32);
        let coins = vec![coin(&format!("{funding}:1"))];
        assert!(
            !history_covers(&coins, &[]),
            "a coin published before its transaction leaves history incomplete"
        );
        assert!(
            !history_covers(&coins, &[tx(&"bb".repeat(32))]),
            "another transaction does not cover the coin"
        );
        assert!(
            history_covers(&coins, &[tx(&funding)]),
            "the written funding transaction completes the history"
        );
        assert!(history_covers(&[], &[]), "no coin needs no transaction");
    }
}
