//! Funded backend contracts against isolated Bitcoin Core, electrs and BlindBit.

mod common;

use std::{
    fs,
    io::{BufRead, BufReader},
    path::Path,
    process::{Child, Command, Stdio},
    str::FromStr,
    thread,
    time::{Duration, Instant},
};

use bitcoin::{
    bip32::{Xpriv, Xpub},
    secp256k1::Secp256k1,
    Address, BlockHash, Txid,
};
use bwk_utils::test as bwk_test;
use silent::{
    config_from_file, keys::crypto::Password, new_account, new_config, new_musig_account,
    state_crypt, test_and_save_endpoint, BackendServer, Network, RustTxDetails, MUSIG_PLUGIN_ID,
};

struct TlsProxy {
    process: Child,
    url: String,
    _directory: temp_dir::TempDir,
}

impl TlsProxy {
    fn new(endpoint: &str) -> Self {
        let directory = temp_dir::TempDir::new().expect("TLS test directory");
        let cert = directory.path().join("cert.pem");
        let key = directory.path().join("key.pem");
        let generated = Command::new("openssl")
            .args([
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-nodes",
                "-days",
                "1",
                "-subj",
                "/CN=localhost",
                "-keyout",
            ])
            .arg(&key)
            .arg("-out")
            .arg(&cert)
            .output()
            .expect("generate local TLS certificate");
        assert!(
            generated.status.success(),
            "certificate generation: {}",
            String::from_utf8_lossy(&generated.stderr)
        );
        let mut process = Command::new("python3")
            .args([
                "-I",
                "-u",
                "-c",
                r#"
import socket, ssl, sys, threading
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(sys.argv[1], sys.argv[2])
host, port = sys.argv[3].rsplit(':', 1)
listener = socket.socket()
listener.bind(('127.0.0.1', 0))
listener.listen()
print(listener.getsockname()[1], flush=True)
def copy(source, target):
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            target.sendall(data)
    except OSError:
        pass
    finally:
        try:
            target.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
def serve(raw):
    try:
        with context.wrap_socket(raw, server_side=True) as client:
            with socket.create_connection((host, int(port))) as upstream:
                reverse = threading.Thread(target=copy, args=(upstream, client), daemon=True)
                reverse.start()
                copy(client, upstream)
                reverse.join()
    except OSError:
        raw.close()
while True:
    raw, _ = listener.accept()
    threading.Thread(target=serve, args=(raw,), daemon=True).start()
"#,
            ])
            .arg(&cert)
            .arg(&key)
            .arg(endpoint)
            .stdout(Stdio::piped())
            .spawn()
            .expect("local TLS proxy");
        let mut port = String::new();
        BufReader::new(process.stdout.take().expect("proxy stdout"))
            .read_line(&mut port)
            .expect("proxy address");
        let port: u16 = port.trim().parse().expect("proxy listening port");
        Self {
            process,
            url: format!("ssl://127.0.0.1:{port}"),
            _directory: directory,
        }
    }
}

impl Drop for TlsProxy {
    fn drop(&mut self) {
        let _ = self.process.kill();
        let _ = self.process.wait();
    }
}

fn wait_details(mut details: impl FnMut() -> RustTxDetails) -> RustTxDetails {
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let result = details();
        if result.is_ok && result.tx.timestamp > 0 && result.tx.status == "verified" {
            return result;
        }
        assert!(
            Instant::now() < deadline,
            "confirmation timed out: {result:?}"
        );
        thread::sleep(Duration::from_millis(100));
    }
}

fn assert_chain_details(node: &mut bwk_test::corepc_node::Client, details: &RustTxDetails) {
    let txid = Txid::from_str(&details.tx.txid).expect("real transaction id");
    let tx = bwk_test::get_tx(node, txid).expect("funded transaction");
    let hash = node
        .get_raw_transaction_verbose(txid)
        .expect("confirmed tx")
        .block_hash
        .expect("confirming block hash");
    let block = node
        .get_block(BlockHash::from_str(&hash).expect("block hash"))
        .expect("confirming block");
    assert_eq!(
        details.tx.timestamp,
        u64::from(block.header.time),
        "actual chain date"
    );
    assert_eq!(
        details.tx.height as u64,
        block.bip34_block_height().expect("height"),
        "actual chain height"
    );
    assert_eq!(
        details.outputs.len(),
        tx.output.len(),
        "every real output is exposed"
    );
    for (io, output) in details.outputs.iter().zip(&tx.output) {
        assert_eq!(io.value, output.value.to_sat(), "exact output amount");
        assert_eq!(
            io.script,
            hex::encode(output.script_pubkey.as_bytes()),
            "actual script"
        );
    }
    assert!(
        details.input_total_known && details.fee_known,
        "resolved actual prevouts"
    );
    let input_total: u64 = tx
        .input
        .iter()
        .map(|input| {
            let prev =
                bwk_test::get_tx(node, input.previous_output.txid).expect("funding transaction");
            prev.output[input.previous_output.vout as usize]
                .value
                .to_sat()
        })
        .sum();
    let output_total: u64 = tx.output.iter().map(|output| output.value.to_sat()).sum();
    assert_eq!(details.input_total, input_total, "actual prevout sum");
    assert_eq!(details.output_total, output_total, "actual output sum");
    assert_eq!(details.fee, input_total - output_total, "actual fee");
    assert_eq!(
        details.fee_rate,
        details.fee as f64 / tx.vsize() as f64,
        "actual fee rate"
    );
    if details.chain_tip_known {
        assert!(details.chain_tip >= details.tx.height, "real chain tip");
    }
}

fn assert_sealed_files(path: &Path) {
    for entry in fs::read_dir(path).expect("account state directory") {
        let path = entry.expect("state entry").path();
        if path.is_dir() {
            assert_sealed_files(&path);
        } else if path
            .extension()
            .is_some_and(|extension| extension == "bip138")
        {
            assert!(
                state_crypt::is_sealed(&fs::read(&path).expect("state bytes")),
                "encrypted {}",
                path.display()
            );
        }
    }
}

#[test]
#[ignore = "requires local bitcoind, electrs, openssl and python3"]
fn funded_details_labels_dates_and_password_reopen_for_both_backends() {
    let musig_name = common::test_account_name();
    let sp_name = common::test_account_name();
    state_crypt::create_with_password(Password::new("backend test password".into()))
        .expect("isolated password-encrypted state");
    let (bbd, mut node, endpoint) = common::setup_blindbitd_with_electrum();
    let proxy = TlsProxy::new(&endpoint);
    bwk_test::generate_blocks(&mut node.client, 101);
    let secp = Secp256k1::new();
    let keys: Vec<_> = [1, 2]
        .into_iter()
        .map(|seed| {
            let key = Xpriv::new_master(bitcoin::Network::Regtest, &[seed; 32]).expect("master");
            format!(
                "[{}]{}",
                key.fingerprint(&secp),
                Xpub::from_priv(&secp, &key)
            )
        })
        .collect();
    let config = new_config(
        musig_name.clone(),
        Network::Regtest,
        format!("tr(musig({},{})/<0;1>/*)", keys[0], keys[1]),
        String::new(),
        String::new(),
        0,
        MUSIG_PLUGIN_ID.into(),
    );
    assert!(config.save().is_empty(), "save Multikey account");
    let mut musig = new_musig_account(musig_name.clone());
    assert!(musig.is_ok(), "open Multikey: {}", musig.get_error());
    let rejected = test_and_save_endpoint(
        musig_name.clone(),
        BackendServer::Electrum,
        proxy.url.clone(),
        true,
    );
    assert!(
        !rejected.is_ok && !rejected.saved,
        "self-signed certificate refused when verification is enabled"
    );
    let saved = test_and_save_endpoint(
        musig_name.clone(),
        BackendServer::Electrum,
        proxy.url.clone(),
        false,
    );
    assert!(
        saved.is_ok && saved.saved,
        "save TLS endpoint with explicit certificate opt-out: {}",
        saved.error
    );
    assert!(musig.reopen_backend(false).is_ok, "save does not connect");
    let connected = musig.reopen_backend(true);
    assert!(
        connected.is_ok,
        "actual Multikey scanner connects through TLS: {}",
        connected.error
    );
    let address = Address::from_str(&musig.new_addr())
        .expect("receive address")
        .require_network(bitcoin::Network::Regtest)
        .expect("regtest");
    let txid = bwk_test::send(&mut node.client, address, 0.001).expect("fund Multikey");
    bwk_test::generate_blocks(&mut node.client, 1);
    let details = wait_details(|| musig.tx_details(&txid.to_string(), true));
    assert_chain_details(&mut node.client, &details);
    let owned = details
        .outputs
        .iter()
        .find(|io| io.is_mine)
        .expect("owned output");
    assert!(
        musig
            .update_coin_label(owned.outpoint.clone(), "Multikey output".into())
            .is_ok,
        "label output"
    );
    assert!(
        musig
            .update_tx_label(txid.to_string(), "Multikey funding".into())
            .is_ok,
        "label transaction"
    );
    let offline = musig.tx_details(&txid.to_string(), false);
    assert!(
        !offline.fee_known && !offline.input_total_known,
        "network prevouts are not fabricated offline"
    );
    assert_eq!(
        musig.coins()[0].timestamp,
        details.tx.timestamp,
        "coin uses chain date"
    );
    drop(musig);

    let sp_config = new_config(
        sp_name.clone(),
        Network::Regtest,
        common::descriptor_from_mnemonic(common::TEST_MNEMONIC, Network::Regtest),
        bbd.url(),
        String::new(),
        0,
        "sp".into(),
    );
    assert!(sp_config.save().is_empty(), "save SP account");
    let height = common::fund_sp_wallet(&mut node.client, &bbd.url(), common::TEST_MNEMONIC, 0.002);
    let mut sp = new_account(sp_name.clone());
    let saved = test_and_save_endpoint(
        sp_name.clone(),
        BackendServer::Electrum,
        proxy.url.clone(),
        false,
    );
    assert!(
        saved.is_ok && saved.saved,
        "save SP TLS endpoint: {}",
        saved.error
    );
    let connected = sp.reopen_backend(true);
    assert!(
        connected.is_ok,
        "actual SP header scanner connects through TLS: {}",
        connected.error
    );
    assert!(
        sp.is_ok() && sp.start_scanner(),
        "start funded SP scanner: {}",
        sp.get_error()
    );
    assert!(
        common::wait_for_scan_complete(&mut sp, 60),
        "SP scan completes"
    );
    let coin = sp
        .coins()
        .into_iter()
        .find(|coin| coin.height == height)
        .expect("funded SP coin");
    let sp_txid = coin
        .outpoint
        .split(':')
        .next()
        .expect("outpoint txid")
        .to_string();
    let sp_details = wait_details(|| sp.tx_details(&sp_txid, true));
    assert_chain_details(&mut node.client, &sp_details);
    assert!(
        sp.update_coin_label(coin.outpoint.clone(), "SP output".into())
            .is_ok,
        "label SP output"
    );
    assert!(
        sp.update_tx_label(sp_txid.clone(), "SP funding".into())
            .is_ok,
        "label SP tx"
    );
    assert_eq!(
        sp.coins()[0].timestamp,
        sp_details.tx.timestamp,
        "SP coin date"
    );
    sp.stop_scanner();
    drop(sp);

    assert_sealed_files(&config.account_dir());
    assert_sealed_files(&sp_config.account_dir());
    state_crypt::lock();
    assert!(
        !config_from_file(musig_name.clone()).is_ok(),
        "locked account is unreadable"
    );
    assert!(
        state_crypt::unlock_with_password(Password::new("wrong password".into())).is_err(),
        "wrong password refused"
    );
    state_crypt::unlock_with_password(Password::new("backend test password".into()))
        .expect("unlock persisted state");
    let musig = new_musig_account(musig_name.clone());
    let details = musig.tx_details(&txid.to_string(), false);
    assert!(
        details.is_ok,
        "reopened Multikey details: {}",
        details.error
    );
    assert_eq!(
        details.tx.label, "Multikey funding",
        "encrypted history label persists"
    );
    assert!(
        details
            .outputs
            .iter()
            .any(|io| io.label == "Multikey output"),
        "encrypted outpoint label persists"
    );
    let sp = new_account(sp_name.clone());
    let details = sp.tx_details(&sp_txid, true);
    assert!(details.is_ok, "reopened SP details: {}", details.error);
    assert_eq!(details.tx.label, "SP funding", "SP history label persists");
    assert!(
        details.outputs.iter().any(|io| io.label == "SP output"),
        "SP outpoint label persists"
    );
    drop(musig);
    drop(sp);
    common::cleanup_test_account(&musig_name);
    common::cleanup_test_account(&sp_name);
}
