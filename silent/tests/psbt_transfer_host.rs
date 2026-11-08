//! Run with the built Qt host: a Multikey account imports a cosigner's base64 PSBT from
//! the clipboard on its PSBT tab, and the saved row exports it back to the clipboard.

mod common;

use std::{env, fs, net::TcpListener, path::PathBuf, process::Command};

use bitcoin::{
    absolute::LockTime, hashes::Hash, transaction::Version, Amount, OutPoint, Transaction, TxIn,
    TxOut, Txid,
};
use bwk::account::Account as BwkAccount;
use silent::{
    keys::crypto::Password, new_config, set_datadir, state_crypt, MusigAccount, Network, Output,
    TransactionTemplate, MUSIG_PLUGIN_ID,
};
use temp_dir::TempDir;

const FUNDING: u64 = 100_000;
const DESCRIPTOR: &str = "tr(musig([4ba43603/87'/1'/0'/2]tpubDFGmrfdE8Bya4CPLde4Q54gxkY4KKJzzs6y5tGWxVEcrfmzZb17aCamvX9k1UwKPPHtd7u5WqyS5SiRwxBZvHhbCJumqwECQQycuAmYtfzZ,[8dfc9b34/87'/1'/0'/2]tpubDFWeBGy8cbQXdCA37ePK189L12ML8fZCuuG9EZtwybxBx2jrVRLrWwsXvRHvXuCS5Q6sRyLB92TsidhBVbwWoeTAxDsDC2w5vQmPVnL2vKB)/<0;1>/*,pk([56c4fac3/87'/1'/0'/2]tpubDE1s5mmJDMZQ9uwi2KHe99ioqqEpwwZbAmAWP1UK6Y6zHLTKQwhkGLeSsQi38VaBwzfWtUrSL7v4tD1DPmHKjSfXBRbXVYUVboM7idebk7T/<2;3>/*))";

/// Saves a Multikey account on `electrum_url` whose wallet holds the one unconfirmed coin
/// of FUNDING sats every account of DESCRIPTOR records, and answers its outpoint.
fn funded_account(name: &str, electrum_url: &str) -> String {
    let mut config = new_config(
        name.into(),
        Network::Regtest,
        DESCRIPTOR.into(),
        String::new(),
        String::new(),
        0,
        MUSIG_PLUGIN_ID.into(),
    );
    assert!(config.save().is_empty(), "save the {name} account");
    let mut bwk: BwkAccount =
        BwkAccount::try_new(config.to_musig_config().expect("backend config")).expect("backend");
    let receive = bwk.scanner_mut().new_recv_addr();
    let funding = Transaction {
        version: Version::TWO,
        lock_time: LockTime::ZERO,
        input: vec![TxIn {
            previous_output: OutPoint::new(Txid::from_byte_array([83; 32]), 0),
            ..Default::default()
        }],
        output: vec![TxOut {
            value: Amount::from_sat(FUNDING),
            script_pubkey: receive.script_pubkey(),
        }],
    };
    bwk.scanner().record_unconfirmed_spend(&funding);
    drop(bwk);
    config.set_electrum_url(electrum_url.into());
    assert!(config.save().is_empty(), "save the Electrum endpoint");
    OutPoint::new(funding.compute_txid(), 0).to_string()
}

/// The cosigner's saved PSBT of a payment from `outpoint`, as base64, and its txid.
fn cosigner_psbt(account: &MusigAccount, outpoint: String) -> (String, String) {
    let paths = account.spend_leaves(vec![outpoint.clone()]);
    assert!(paths.is_ok, "spend paths: {}", paths.error);
    let prepared = common::musig_prepared(
        account,
        TransactionTemplate {
            outputs: vec![Output {
                address: "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080".into(),
                amount: 40_000,
                label: String::new(),
                max: false,
            }],
            fee: 1000,
            fee_rate: 0.0,
            input_outpoints: vec![outpoint],
        },
        paths.leaves[0].clone(),
    );
    assert!(
        prepared.is_ok(),
        "prepare the cosigner's PSBT: {}",
        prepared.get_psbt_error()
    );
    let exported =
        silent::export_psbt(&hex::decode(prepared.bytes_v2_hex()).expect("prepared hex"));
    assert!(exported.is_ok, "export: {}", exported.error);
    (exported.base64, exported.txid)
}

#[test]
#[ignore = "requires SILENT_HOST_SMOKE"]
fn multikey_imports_and_exports_a_cosigner_psbt_on_its_psbt_tab() {
    let host = PathBuf::from(env::var_os("SILENT_HOST_SMOKE").expect("built host_smoke path"));
    let home = TempDir::new().expect("isolated host HOME");
    set_datadir(home.path().join(".silent"));
    state_crypt::create_with_password(Password::new("host smoke".into()))
        .expect("create the encrypted state the Qt host unlocks");
    // Connections queue on the listener and are never accepted: the account stays
    // offline, its coins read from its own store.
    let server = TcpListener::bind("127.0.0.1:0").expect("bind the silent server");
    let electrum = format!(
        "tcp://127.0.0.1:{}",
        server.local_addr().expect("server address").port()
    );
    let cosigner = format!("psbt_cosigner_{}", std::process::id());
    let name = format!("psbt_importer_{}", std::process::id());
    let outpoint = funded_account(&cosigner, &electrum);
    assert_eq!(
        funded_account(&name, &electrum),
        outpoint,
        "both accounts hold the same coin"
    );
    let account = silent::new_musig_account(cosigner.clone());
    assert!(
        account.is_ok(),
        "open the cosigner: {}",
        account.get_error()
    );
    let (base64, txid) = cosigner_psbt(&account, outpoint.clone());
    drop(account);
    let file = home.path().join("cosigner.psbt");
    fs::write(&file, &base64).expect("write the cosigner's PSBT");
    state_crypt::lock();
    let output = Command::new(&host)
        .arg("--psbt-transfer")
        .arg(&name)
        .arg(&file)
        .arg(&txid)
        .arg(&outpoint)
        .env("HOME", home.path())
        .env("QT_QPA_PLATFORM", "offscreen")
        .output()
        .expect("execute the built Qt host");
    drop(server);
    assert!(
        output.status.success(),
        "PSBT transfer failed:\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(
        String::from_utf8_lossy(&output.stdout).contains("PSBT transfer smoke: OK"),
        "the PSBT transfer smoke ran to completion"
    );
}
