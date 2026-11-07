//! Run with the built Qt host: a Multikey account selects its coins while its Electrum
//! server never answers, during the startup connection and during a reconnect.

use std::{env, net::TcpListener, path::PathBuf, process::Command};

use bitcoin::{
    absolute::LockTime, hashes::Hash, transaction::Version, Amount, OutPoint, Transaction, TxIn,
    TxOut, Txid,
};
use bwk::account::Account as BwkAccount;
use silent::{
    keys::crypto::Password, new_config, set_datadir, state_crypt, Network, MUSIG_PLUGIN_ID,
};
use temp_dir::TempDir;

const FUNDING: u64 = 100_000;
const DESCRIPTOR: &str = "tr(musig([4ba43603/87'/1'/0'/2]tpubDFGmrfdE8Bya4CPLde4Q54gxkY4KKJzzs6y5tGWxVEcrfmzZb17aCamvX9k1UwKPPHtd7u5WqyS5SiRwxBZvHhbCJumqwECQQycuAmYtfzZ,[8dfc9b34/87'/1'/0'/2]tpubDFWeBGy8cbQXdCA37ePK189L12ML8fZCuuG9EZtwybxBx2jrVRLrWwsXvRHvXuCS5Q6sRyLB92TsidhBVbwWoeTAxDsDC2w5vQmPVnL2vKB)/<0;1>/*,pk([56c4fac3/87'/1'/0'/2]tpubDE1s5mmJDMZQ9uwi2KHe99ioqqEpwwZbAmAWP1UK6Y6zHLTKQwhkGLeSsQi38VaBwzfWtUrSL7v4tD1DPmHKjSfXBRbXVYUVboM7idebk7T/<2;3>/*))";

/// Saves a Multikey account on `electrum_url` whose wallet holds one unconfirmed coin of
/// FUNDING sats, recorded offline, and answers that coin's outpoint.
fn funded_account(name: &str, electrum_url: String) -> String {
    let mut config = new_config(
        name.into(),
        Network::Regtest,
        DESCRIPTOR.into(),
        String::new(),
        String::new(),
        0,
        MUSIG_PLUGIN_ID.into(),
    );
    assert!(
        config.save().is_empty(),
        "save the Multikey fixture account"
    );
    let mut bwk: BwkAccount =
        BwkAccount::try_new(config.to_musig_config().expect("backend config")).expect("backend");
    let receive = bwk.scanner_mut().new_recv_addr();
    let funding = Transaction {
        version: Version::TWO,
        lock_time: LockTime::ZERO,
        input: vec![TxIn {
            previous_output: OutPoint::new(Txid::from_byte_array([82; 32]), 0),
            ..Default::default()
        }],
        output: vec![TxOut {
            value: Amount::from_sat(FUNDING),
            script_pubkey: receive.script_pubkey(),
        }],
    };
    bwk.scanner().record_unconfirmed_spend(&funding);
    drop(bwk);
    config.set_electrum_url(electrum_url);
    assert!(config.save().is_empty(), "save the Electrum endpoint");
    OutPoint::new(funding.compute_txid(), 0).to_string()
}

#[test]
#[ignore = "requires SILENT_HOST_SMOKE"]
fn multikey_selects_its_coin_while_electrum_never_answers() {
    let host = PathBuf::from(env::var_os("SILENT_HOST_SMOKE").expect("built host_smoke path"));
    let home = TempDir::new().expect("isolated host HOME");
    set_datadir(home.path().join(".silent"));
    state_crypt::create_with_password(Password::new("host smoke".into()))
        .expect("create the encrypted state the Qt host unlocks");
    // Connections queue on the listener and are never accepted: every connection attempt
    // waits for a version answer until it times out.
    let silent_server = TcpListener::bind("127.0.0.1:0").expect("bind the silent server");
    let port = silent_server
        .local_addr()
        .expect("silent server address")
        .port();
    let name = format!("offline_selection_{}", std::process::id());
    let outpoint = funded_account(&name, format!("tcp://127.0.0.1:{port}"));
    state_crypt::lock();
    let output = Command::new(&host)
        .arg("--offline-selection")
        .arg(&name)
        .arg(&outpoint)
        .env("HOME", home.path())
        .env("QT_QPA_PLATFORM", "offscreen")
        .output()
        .expect("execute the built Qt host");
    drop(silent_server);
    assert!(
        output.status.success(),
        "offline selection failed:\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(
        String::from_utf8_lossy(&output.stdout).contains("Offline selection smoke: OK"),
        "the offline selection smoke ran to completion"
    );
}
