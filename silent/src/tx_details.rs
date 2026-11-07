//! Shared transaction snapshots. Store guards are dropped before Electrum I/O.

use std::collections::{BTreeMap, BTreeSet};

use bitcoin::{Address, Network, OutPoint, ScriptBuf, Transaction, TxOut, Txid};
use bwk::bwk_electrum::{client::Client, label_store::LabelKey, scanner::ElectrumScanner};

use crate::{
    config::{certificate_check, parse_electrum_url},
    ffi::{RustTx, RustTxDetails, RustTxIo},
};

pub(crate) fn edit_scanner_label(
    scanner: &ElectrumScanner,
    key: LabelKey,
    label: String,
) -> Result<(), String> {
    crate::services::edit_store_label(scanner.label_store(), key, label)
}

impl Default for crate::ffi::Network {
    fn default() -> Self {
        Self::Regtest
    }
}

pub(crate) struct TxSnapshot {
    pub network: Network,
    pub transactions: BTreeMap<Txid, Transaction>,
    pub prevouts: BTreeMap<OutPoint, TxOut>,
    pub owned_scripts: BTreeSet<ScriptBuf>,
    pub history: Vec<RustTx>,
    pub tip: Option<u32>,
}

impl TxSnapshot {
    pub fn new(network: Network, history: Vec<RustTx>, tip: Option<u32>) -> Self {
        Self {
            network,
            transactions: BTreeMap::new(),
            prevouts: BTreeMap::new(),
            owned_scripts: BTreeSet::new(),
            history,
            tip,
        }
    }

    pub fn add_scanner(&mut self, scanner: &ElectrumScanner) {
        for entry in scanner.tx_history() {
            self.transactions.insert(entry.txid(), entry.tx().clone());
        }
        for entry in scanner.address_entries() {
            self.owned_scripts
                .insert(entry.address.assume_checked().script_pubkey());
        }
        for (outpoint, coin) in scanner.coins() {
            let output = TxOut {
                value: bitcoin::Amount::from_sat(coin.amount_sat()),
                script_pubkey: coin.spk(),
            };
            self.owned_scripts.insert(output.script_pubkey.clone());
            self.prevouts.insert(outpoint, output);
        }
    }

    pub fn update_history_fees(&mut self) {
        for summary in &mut self.history {
            let Some(tx) = self
                .transactions
                .values()
                .find(|tx| tx.compute_txid().to_string() == summary.txid)
            else {
                continue;
            };
            if tx.is_coinbase() {
                continue;
            }
            let inputs: Option<u64> = tx.input.iter().try_fold(0_u64, |total, input| {
                let output = self.prevouts.get(&input.previous_output).or_else(|| {
                    self.transactions
                        .get(&input.previous_output.txid)
                        .and_then(|previous| {
                            previous.output.get(input.previous_output.vout as usize)
                        })
                })?;
                total.checked_add(output.value.to_sat())
            });
            let outputs = tx.output.iter().try_fold(0_u64, |total, output| {
                total.checked_add(output.value.to_sat())
            });
            if let Some(fee) = inputs
                .zip(outputs)
                .and_then(|(inputs, outputs)| inputs.checked_sub(outputs))
            {
                summary.fee = fee;
                summary.fee_known = true;
            }
        }
    }

    fn io(&self, index: usize, outpoint: OutPoint, output: Option<&TxOut>) -> RustTxIo {
        let mut io = RustTxIo {
            index: index as u32,
            outpoint: outpoint.to_string(),
            ..RustTxIo::default()
        };
        if let Some(output) = output {
            io.value = output.value.to_sat();
            io.value_known = true;
            io.script = hex::encode(output.script_pubkey.as_bytes());
            io.address = match Address::from_script(&output.script_pubkey, self.network) {
                Ok(address) => address.to_string(),
                Err(_) => String::new(), // OP_RETURN and non-address scripts keep their script.
            };
            io.is_mine = self.owned_scripts.contains(&output.script_pubkey);
            io.ownership_known = true;
        }
        io
    }

    pub fn details(
        mut self,
        txid: &str,
        endpoint: &str,
        verify: bool,
        allow_network: bool,
    ) -> Result<RustTxDetails, String> {
        let txid: Txid = txid.parse().map_err(|e| format!("Invalid txid: {e}"))?;
        let mut client = None;
        if !self.transactions.contains_key(&txid) && allow_network {
            let tx = fetch_transaction(&mut client, endpoint, verify, txid)?;
            self.transactions.insert(txid, tx);
        }
        let tx =
            self.transactions.get(&txid).cloned().ok_or_else(|| {
                "Transaction is not stored and cannot be fetched offline".to_string()
            })?;
        let warning = self.resolve_prevouts(&tx, &mut client, endpoint, verify, allow_network);
        self.build_details(tx, warning)
    }

    fn resolve_prevouts(
        &mut self,
        tx: &Transaction,
        client: &mut Option<Client>,
        endpoint: &str,
        verify: bool,
        allow_network: bool,
    ) -> String {
        let mut warning = String::new();
        for input in &tx.input {
            let outpoint = input.previous_output;
            if outpoint.is_null() || self.prevouts.contains_key(&outpoint) {
                continue;
            }
            if !self.transactions.contains_key(&outpoint.txid) && allow_network {
                let result = fetch_transaction(client, endpoint, verify, outpoint.txid);
                match result {
                    Ok(previous) => {
                        self.transactions.insert(outpoint.txid, previous);
                    }
                    Err(error) => {
                        warning = error;
                    }
                }
            }
            if let Some(output) = self
                .transactions
                .get(&outpoint.txid)
                .and_then(|previous| previous.output.get(outpoint.vout as usize))
            {
                self.prevouts.insert(outpoint, output.clone());
            }
        }
        warning
    }

    fn build_details(self, tx: Transaction, mut warning: String) -> Result<RustTxDetails, String> {
        let txid = tx.compute_txid();
        let inputs: Vec<_> = tx
            .input
            .iter()
            .enumerate()
            .map(|(index, input)| {
                self.io(
                    index,
                    input.previous_output,
                    self.prevouts.get(&input.previous_output),
                )
            })
            .collect();
        let outputs: Vec<_> = tx
            .output
            .iter()
            .enumerate()
            .map(|(index, output)| self.io(index, OutPoint::new(txid, index as u32), Some(output)))
            .collect();
        let input_total_known = !tx.is_coinbase() && inputs.iter().all(|input| input.value_known);
        let input_total = if input_total_known {
            io_total(&inputs)?
        } else {
            0
        };
        let output_total = io_total(&outputs)?;
        let mut fee = if input_total_known {
            Some(
                input_total
                    .checked_sub(output_total)
                    .ok_or("Input total is lower than output total")?,
            )
        } else {
            None
        };
        if !input_total_known && warning.is_empty() {
            warning = "Some input values are unavailable".into();
        }
        let mut summary = self
            .history
            .into_iter()
            .find(|entry| entry.txid == txid.to_string())
            .unwrap_or_else(|| RustTx {
                txid: txid.to_string(),
                status: "unknown".into(),
                ..RustTx::default()
            });
        summarize_ownership(&mut summary, &inputs, &outputs);
        if fee.is_none() && summary.fee_known {
            fee = Some(summary.fee);
        }
        summary.fee_known = fee.is_some();
        summary.fee = fee.unwrap_or(0);
        Ok(RustTxDetails {
            is_ok: true,
            warning,
            tx: summary,
            network: self.network.into(),
            version: tx.version.0,
            lock_time: tx.lock_time.to_consensus_u32(),
            weight: tx.weight().to_wu(),
            vsize: tx.vsize() as u64,
            inputs,
            outputs,
            input_total,
            input_total_known,
            output_total,
            fee: fee.unwrap_or(0),
            fee_known: fee.is_some(),
            fee_rate: fee.map_or(0.0, |fee| fee as f64 / tx.vsize() as f64),
            chain_tip: self.tip.unwrap_or(0),
            chain_tip_known: self.tip.is_some(),
            ..RustTxDetails::default()
        })
    }
}

fn connect(endpoint: &str, verify: bool) -> Result<Client, String> {
    let (Some(host), Some(port)) = parse_electrum_url(endpoint) else {
        return Err("No usable Electrum endpoint".into());
    };
    Client::new(&host, port, certificate_check(verify)).map_err(|e| e.to_string())
}

fn check_txid(tx: &Transaction, expected: Txid) -> Result<(), String> {
    if tx.compute_txid() != expected {
        return Err(format!(
            "Electrum returned the wrong transaction for {expected}"
        ));
    }
    Ok(())
}

fn fetch_transaction(
    client: &mut Option<Client>,
    endpoint: &str,
    verify: bool,
    txid: Txid,
) -> Result<Transaction, String> {
    if client.is_none() {
        *client = Some(connect(endpoint, verify)?);
    }
    let tx = client
        .as_mut()
        .ok_or("Electrum client was not initialized")?
        .get_tx(txid)
        .map_err(|e| format!("Cannot fetch transaction {txid}: {e}"))?;
    check_txid(&tx, txid)?;
    Ok(tx)
}

fn io_total(ios: &[RustTxIo]) -> Result<u64, String> {
    ios.iter()
        .try_fold(0_u64, |total, io| total.checked_add(io.value))
        .ok_or_else(|| "Transaction amount total overflows".into())
}

pub(crate) fn details_result(result: Result<RustTxDetails, String>) -> RustTxDetails {
    match result {
        Ok(details) => details,
        Err(error) => RustTxDetails {
            error,
            ..RustTxDetails::default()
        },
    }
}

fn summarize_ownership(summary: &mut RustTx, inputs: &[RustTxIo], outputs: &[RustTxIo]) {
    if summary.amount_known
        || inputs
            .iter()
            .any(|io| !io.value_known || !io.ownership_known)
    {
        return;
    }
    let owned_in: u64 = inputs
        .iter()
        .filter(|io| io.is_mine)
        .map(|io| io.value)
        .sum();
    let owned_out: u64 = outputs
        .iter()
        .filter(|io| io.is_mine)
        .map(|io| io.value)
        .sum();
    if owned_in == 0 && owned_out == 0 {
        return;
    }
    summary.direction = match (
        owned_in,
        owned_out > owned_in,
        outputs.iter().all(|io| io.is_mine),
    ) {
        (0, _, _) | (_, true, _) => "incoming",
        (_, _, true) => "self",
        _ => "outgoing",
    }
    .into();
    summary.amount = owned_in.abs_diff(owned_out);
    summary.amount_known = true;
}

pub(crate) fn apply_scanner_labels(
    details: &mut RustTxDetails,
    scanner: &ElectrumScanner,
) -> Result<(), String> {
    let labels = scanner.label_store().lock().map_err(|e| e.to_string())?;
    for io in details.inputs.iter_mut().chain(details.outputs.iter_mut()) {
        let outpoint = io.outpoint.parse::<OutPoint>().map_err(|e| e.to_string())?;
        if let Some(label) = labels.outpoint(outpoint) {
            io.label = label;
        }
    }
    let txid = details.tx.txid.parse::<Txid>().map_err(|e| e.to_string())?;
    if let Some(label) = labels.transaction(txid) {
        details.tx.label = label;
    }
    Ok(())
}

pub fn test_and_save_endpoint(
    account_name: String,
    server: crate::ffi::BackendServer,
    url: String,
    verify_certificate: bool,
) -> crate::ffi::EndpointResult {
    let result = (|| {
        let mut config = crate::config::Config::from_file(account_name)
            .map_err(|e| (crate::ffi::BackendErrorKind::Storage, e.to_string()))?;
        if server == crate::ffi::BackendServer::Blindbit
            && config.plugin_id == crate::musig::MUSIG_PLUGIN_ID
        {
            return Err((
                crate::ffi::BackendErrorKind::InvalidInput,
                "This account has no BlindBit endpoint".into(),
            ));
        }
        let url = crate::services::tested_url(server, url, verify_certificate, config.network)?;
        if server == crate::ffi::BackendServer::Electrum {
            config.set_electrum_url(url.clone());
            config.set_verify_certificate(verify_certificate);
        } else {
            config.set_blindbit_url(url.clone());
        }
        config
            .write_file()
            .map_err(|e| (crate::ffi::BackendErrorKind::Storage, e))?;
        Ok(crate::ffi::EndpointResult {
            connection_pending: false,
            kind: crate::ffi::BackendErrorKind::None,
            is_ok: true,
            saved: true,
            error: String::new(),
            url,
            verify_certificate: config.verify_certificate,
        })
    })();
    match result {
        Ok(result) => result,
        Err((kind, error)) => crate::ffi::EndpointResult {
            kind,
            error,
            ..crate::ffi::EndpointResult::default()
        },
    }
}

#[cfg(test)]
mod tests {
    use std::{
        io::{BufRead, BufReader, Write},
        net::TcpListener,
        thread,
        time::Duration,
    };

    use bitcoin::{
        absolute::LockTime, consensus::encode::serialize_hex, hashes::Hash, transaction::Version,
        Amount, Network, OutPoint, ScriptBuf, Transaction, TxIn, TxOut, WPubkeyHash, Witness,
    };
    use bwk::bwk_electrum::coin_store::{Payment, PaymentStatus, PaymentType};

    use crate::{account::rust_txs, ffi::RustTx, tx_details::TxSnapshot};

    fn funding() -> Transaction {
        Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: vec![TxIn::default()],
            output: vec![TxOut {
                value: Amount::from_sat(100_000),
                script_pubkey: ScriptBuf::new_p2wpkh(&WPubkeyHash::from_byte_array([1; 20])),
            }],
        }
    }

    fn spend() -> Transaction {
        Transaction {
            version: Version::TWO,
            lock_time: LockTime::from_height(123).expect("valid height"),
            input: vec![TxIn {
                previous_output: OutPoint::new(funding().compute_txid(), 0),
                witness: Witness::from_slice(&[&[7; 64]]),
                ..TxIn::default()
            }],
            output: vec![
                TxOut {
                    value: Amount::from_sat(60_000),
                    script_pubkey: funding().output[0].script_pubkey.clone(),
                },
                TxOut {
                    value: Amount::from_sat(30_000),
                    script_pubkey: ScriptBuf::new_op_return([1, 2]),
                },
            ],
        }
    }

    fn snapshot(include_funding: bool) -> TxSnapshot {
        let tx = spend();
        let mut snapshot = TxSnapshot::new(
            Network::Regtest,
            vec![RustTx {
                txid: tx.compute_txid().to_string(),
                direction: "outgoing".into(),
                timestamp: 1_700_000_123,
                height: 120,
                label: "Payment".into(),
                status: "verified".into(),
                ..RustTx::default()
            }],
            Some(125),
        );
        snapshot.transactions.insert(tx.compute_txid(), tx.clone());
        if include_funding {
            snapshot
                .transactions
                .insert(funding().compute_txid(), funding());
        }
        snapshot
            .owned_scripts
            .insert(funding().output[0].script_pubkey.clone());
        snapshot
    }

    #[test]
    fn details_keep_real_metadata_and_exact_flow() {
        let tx = spend();
        let details = snapshot(true)
            .details(&tx.compute_txid().to_string(), "", true, false)
            .expect("stored details");
        assert_eq!(details.version, 2, "actual version");
        assert_eq!(details.lock_time, 123, "actual locktime");
        assert_eq!(details.weight, tx.weight().to_wu(), "includes real witness");
        assert_eq!(details.vsize, tx.vsize() as u64, "actual virtual size");
        assert_eq!(details.tx.timestamp, 1_700_000_123, "confirming timestamp");
        assert_eq!(details.tx.label, "Payment", "history label");
        assert_eq!(details.tx.status, "verified", "actual verification status");
        assert!(details.chain_tip_known, "stored chain tip available");
        assert_eq!(details.chain_tip, 125, "real chain tip");
        assert!(
            details.input_total_known && details.fee_known,
            "funding is stored"
        );
        assert_eq!(details.input_total, 100_000, "exact input sum");
        assert_eq!(details.output_total, 90_000, "exact output sum");
        assert_eq!(details.fee, 10_000, "exact transaction fee");
        assert_eq!(
            details.fee_rate,
            10_000.0 / tx.vsize() as f64,
            "actual fee rate"
        );
        assert_eq!(
            details.inputs[0].outpoint,
            tx.input[0].previous_output.to_string(),
            "input outpoint"
        );
        assert!(details.inputs[0].is_mine, "stored funding is owned");
        assert!(details.outputs[0].is_mine, "known change script is owned");
        assert_eq!(details.outputs[1].index, 1, "actual vout index");
        assert!(!details.outputs[1].is_mine, "OP_RETURN is not owned");
        assert!(
            details.outputs[1].address.is_empty(),
            "no fabricated address for OP_RETURN"
        );
        assert_eq!(
            details.outputs[1].script,
            hex::encode(tx.output[1].script_pubkey.as_bytes()),
            "actual script"
        );
    }

    #[test]
    fn offline_missing_prevouts_keep_amount_and_fee_unknown() {
        let details = snapshot(false)
            .details(
                &spend().compute_txid().to_string(),
                "not a server",
                true,
                false,
            )
            .expect("known transaction remains displayable offline");
        assert!(
            !details.input_total_known && !details.fee_known,
            "missing funding is not zero-valued funding"
        );
        assert!(
            !details.inputs[0].value_known,
            "input value is explicitly unknown"
        );
        assert!(
            details.inputs[0].address.is_empty() && details.inputs[0].script.is_empty(),
            "no invented prevout"
        );
        assert!(
            !details.warning.is_empty(),
            "partial details explain unavailable data"
        );
        assert_eq!(
            details.output_total, 90_000,
            "known output total remains exact"
        );
    }

    #[test]
    fn saved_funding_resolves_without_fetching() {
        let mut snapshot = snapshot(false);
        snapshot.prevouts.insert(
            spend().input[0].previous_output,
            funding().output[0].clone(),
        );
        let details = snapshot
            .details(
                &spend().compute_txid().to_string(),
                "not a server",
                true,
                true,
            )
            .expect("saved PSBT funding avoids a network request");
        assert!(details.fee_known, "PSBT prevout provides the exact fee");
        assert_eq!(details.fee, 10_000, "PSBT funding fee");
    }

    fn prevout_server(wrong_tx: bool) -> (String, thread::JoinHandle<()>) {
        let listener = TcpListener::bind("127.0.0.1:0").expect("isolated local server");
        let endpoint = listener.local_addr().expect("server address").to_string();
        let thread = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("client connects");
            stream
                .set_read_timeout(Some(Duration::from_secs(5)))
                .expect("read timeout");
            let mut request = String::new();
            BufReader::new(stream.try_clone().expect("clone socket"))
                .read_line(&mut request)
                .expect("get_tx request");
            let request: serde_json::Value = serde_json::from_str(&request).expect("JSON request");
            assert_eq!(
                request["method"], "blockchain.transaction.get",
                "real Electrum get_tx API"
            );
            assert_eq!(
                request["params"][0],
                funding().compute_txid().to_string(),
                "requested missing prevout tx"
            );
            let tx = if wrong_tx { spend() } else { funding() };
            let response = serde_json::json!({"jsonrpc":"2.0", "id":request["id"], "result":serialize_hex(&tx)});
            writeln!(stream, "{response}").expect("send raw transaction");
        });
        (endpoint, thread)
    }

    #[test]
    fn missing_prevout_is_fetched_and_wrong_transaction_is_refused() {
        for wrong_tx in [false, true] {
            let (endpoint, server) = prevout_server(wrong_tx);
            let details = snapshot(false)
                .details(&spend().compute_txid().to_string(), &endpoint, true, true)
                .expect("transaction remains displayable");
            server.join().expect("local server finished");
            assert_eq!(
                details.fee_known, !wrong_tx,
                "fee availability for wrong_tx={wrong_tx}"
            );
            if wrong_tx {
                assert!(
                    details.warning.contains("wrong transaction"),
                    "wrong response is reported: {}",
                    details.warning
                );
            } else {
                assert_eq!(details.fee, 10_000, "fetched exact fee");
            }
        }
    }

    #[test]
    fn missing_transaction_and_invalid_id_are_errors() {
        for id in ["bad-id".to_string(), funding().compute_txid().to_string()] {
            assert!(
                snapshot(false).details(&id, "", true, false).is_err(),
                "no invented transaction for {id}"
            );
        }
    }

    #[test]
    fn history_keeps_self_labels_timestamps_and_unconfirmed_first() {
        let payments = vec![
            Payment {
                txid: "self".into(),
                payment_type: PaymentType::ToSelf,
                status: PaymentStatus::Verified,
                amount: 100,
                label: "Consolidation".into(),
                height: Some(12),
                timestamp: Some(1_700_000_000),
            },
            Payment {
                txid: "incoming".into(),
                payment_type: PaymentType::Receive,
                status: PaymentStatus::Unconfirmed,
                amount: 200,
                label: String::new(),
                height: None,
                timestamp: None,
            },
        ];
        let history = rust_txs(payments);
        assert_eq!(
            history[0].txid, "incoming",
            "unconfirmed precedes confirmed"
        );
        assert_eq!(history[0].timestamp, 0, "no fabricated mempool date");
        assert_eq!(history[1].direction, "self", "self is not outgoing");
        assert_eq!(history[1].label, "Consolidation", "actual history label");
        assert_eq!(history[1].timestamp, 1_700_000_000, "real confirming time");
        assert!(
            !history[1].fee_known,
            "payment summary does not invent a zero fee"
        );
    }
}
