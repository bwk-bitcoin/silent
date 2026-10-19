//! Shared test helpers for Silent wallet integration tests.
//!
//! Provides BlindbitD setup, wait-for-sync, and SP transaction funding helpers.

#![allow(dead_code)]

use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::Duration;

use bitcoin::bip32::{ChildNumber, DerivationPath};
use bitcoin::{Amount, OutPoint, ScriptBuf, TxOut, XOnlyPublicKey};
use blindbitd::BlindbitD;
use bwk_sp::{
    bwk_sign::{
        bwk_descriptor::{
            descriptor::{tr_path, DescriptorDerivator},
            sp_descriptor::SpDescriptor,
            sp_key::{SpKey, SpScanKey},
        },
        hot_signer::HotSigner,
    },
    receiver::{bip352_base_derivation, bip39, scan_path, spend_path, SpReceiver},
};
use bwk_utils::test as bwk_test;
use bwk_utils::test::corepc_node;

use silent::{Account, Config, Network};

// ===== Constants =====

/// Standard test mnemonic (BIP39 test vector).
pub const TEST_MNEMONIC: &str =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

/// Dust threshold for SP outputs.
const DUST: u64 = 330;

/// A watch-only sp() descriptor built the same way `hot::sp_descriptor` does: the
/// private scan key and the public spend key, no spend secret. `Account::new`
/// refuses any descriptor carrying a spend secret, so every test account is built
/// from this watch-only shape.
pub fn descriptor_from_mnemonic(mnemonic: &str, network: Network) -> String {
    let network = match network {
        Network::Regtest => bitcoin::Network::Regtest,
        Network::Signet => bitcoin::Network::Signet,
        Network::Testnet => bitcoin::Network::Testnet,
        Network::Bitcoin => bitcoin::Network::Bitcoin,
        _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
    };
    let signer = HotSigner::new_from_mnemonics(network, mnemonic).expect("valid signer");
    let account = ChildNumber::from_hardened_idx(0).expect("valid account index");
    let origin = (
        signer.fingerprint(),
        DerivationPath::from(bip352_base_derivation(network, account)),
    );
    SpDescriptor::Packed {
        origin: Some(origin),
        key: SpKey::Scan(SpScanKey {
            scan_key: signer.private_key_at(&scan_path(network, account)),
            spend_key: signer.public_key_at(&spend_path(network, account)),
            network: bitcoin::NetworkKind::from(network),
        }),
    }
    .to_string()
}

// ===== Setup Helpers =====

/// Create a BlindbitD instance and generate initial blocks for coinbase maturity.
/// Returns (BlindbitD, bitcoind_node).
pub fn setup_blindbitd() -> (BlindbitD, corepc_node::Node) {
    let mut bbd = BlindbitD::new().expect("failed to start BlindbitD");
    let bitcoind_node = bbd.bitcoin().expect("failed to get bitcoind");
    (bbd, bitcoind_node)
}

/// Create a Silent Account pointing at the given BlindbitD URL.
pub fn create_test_account(account_name: &str, blindbit_url: &str) -> Account {
    let config = Config::new(
        account_name.to_string(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        blindbit_url.to_string(),
        String::new(),
        Some(546),
    );
    Account::new(config).expect("Account creation should succeed")
}

/// Generate unique test account name.
pub fn test_account_name() -> String {
    isolated_state();
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let seq = COUNTER.fetch_add(1, Ordering::Relaxed);
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .expect("system time should be after epoch")
        .as_nanos();
    format!("test_account_{}_{}_{}", std::process::id(), nanos, seq)
}

/// Points the state at a temporary directory for the whole test binary and unlocks
/// it with the test key, so no test writes into the user's `~/.silent`.
pub fn isolated_state() {
    static STATE_DIR: std::sync::OnceLock<temp_dir::TempDir> = std::sync::OnceLock::new();
    let dir = STATE_DIR.get_or_init(|| temp_dir::TempDir::new().expect("create temp dir"));
    silent::set_datadir(dir.path().to_path_buf());
    silent::state_crypt::unlock_for_tests();
}

/// Cleanup test account directory.
pub fn cleanup_test_account(account_name: &str) {
    let config = Config::from_file(account_name.to_string()).ok();
    if let Some(cfg) = config {
        let _ = std::fs::remove_dir_all(cfg.account_dir());
    }
}

// ===== Sync Helpers =====

/// Wait until BlindbitD has synced to at least the given block height.
/// Polls via `bwk_sp::account::backend_info()` every 2s, with 60s timeout.
pub fn wait_for_sync(url: &str, height: u32) {
    let start = std::time::Instant::now();
    let timeout = Duration::from_secs(60);
    loop {
        if start.elapsed() > timeout {
            panic!("wait_for_sync: timed out waiting for height {height}");
        }
        if let Ok((info, _)) = bwk_sp::account::backend_info(url.to_string()) {
            if info.height.to_consensus_u32() >= height {
                return;
            }
        }
        thread::sleep(Duration::from_secs(2));
    }
}

/// Wait for sync and add extra delay for indexing.
pub fn wait_for_sync_and_index(url: &str, height: u32) {
    wait_for_sync(url, height);
    thread::sleep(Duration::from_secs(2));
}

/// Poll notifications from an Account until a matching flag is found, with timeout.
/// Returns true if the expected notification was received.
pub fn wait_for_notification(
    account: &mut Account,
    flag: silent::NotificationFlag,
    timeout_secs: u64,
) -> bool {
    let start = std::time::Instant::now();
    let timeout = Duration::from_secs(timeout_secs);
    loop {
        if start.elapsed() > timeout {
            return false;
        }
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if notif.flag == flag {
                return true;
            }
        }
        thread::sleep(Duration::from_millis(100));
    }
}

/// Poll notifications, collecting all until ScanCompleted or timeout.
/// Returns true if scan completed.
pub fn wait_for_scan_complete(account: &mut Account, timeout_secs: u64) -> bool {
    let start = std::time::Instant::now();
    let timeout = Duration::from_secs(timeout_secs);
    loop {
        if start.elapsed() > timeout {
            return false;
        }
        let poll = account.try_recv();
        if poll.is_some() {
            let notif = poll.get_notification();
            if matches!(
                notif.flag,
                silent::NotificationFlag::ScanCompleted
                    | silent::NotificationFlag::WaitingForBlocks
            ) {
                return true;
            }
        }
        thread::sleep(Duration::from_millis(100));
    }
}

// ===== SP Transaction Helpers =====

/// Extract the XOnlyPublicKey from a P2TR output script.
fn get_taproot_pubkey(txout: &TxOut) -> XOnlyPublicKey {
    let script_bytes = txout.script_pubkey.as_bytes();
    assert_eq!(script_bytes[0], 0x51); // OP_1
    assert_eq!(script_bytes[1], 0x20); // 32 bytes
    bitcoin::key::XOnlyPublicKey::from_slice(&script_bytes[2..34]).expect("valid output key")
}

/// Generate a recipient public key for a Silent Payment transaction.
#[allow(non_snake_case)]
fn generate_recipient_pubkey(
    sk: bitcoin::secp256k1::SecretKey,
    outpoint: OutPoint,
    txout: &TxOut,
    sp_addr: bwk_sp::core::utils::common::SilentPaymentAddress,
    secp: &bitcoin::secp256k1::Secp256k1<bitcoin::secp256k1::All>,
) -> Option<XOnlyPublicKey> {
    use bitcoin::key::TapTweak;

    let keypair = bitcoin::secp256k1::Keypair::from_secret_key(secp, &sk);
    #[allow(deprecated)]
    let keypair = keypair.tap_tweak(secp, None).to_inner();
    let taproot_pubkey = get_taproot_pubkey(txout);

    let (sp_pk, _parity) = keypair.x_only_public_key();
    assert_eq!(taproot_pubkey, sp_pk);

    let sp_sk = keypair.secret_key();
    let input_keys = vec![(sp_sk, true)];
    let outpoints = vec![outpoint];

    let partial_secret =
        bwk_sp::core::sending::calculate_partial_secret(&input_keys, &outpoints).ok()?;

    bwk_sp::core::sending::generate_recipient_pubkeys(vec![sp_addr], partial_secret)
        .ok()?
        .into_iter()
        .next()
        .and_then(|(_addr, k)| k.into_iter().next())
}

/// Build and sign a transaction that sends to a Silent Payment output.
fn swap_to_sp(
    sk: bitcoin::secp256k1::SecretKey,
    outpoint: OutPoint,
    txout: TxOut,
    recipient_pubkey: XOnlyPublicKey,
    fees: Amount,
    secp: &bitcoin::secp256k1::Secp256k1<bitcoin::secp256k1::All>,
) -> Option<bitcoin::Transaction> {
    use bitcoin::key::TapTweak;
    use bitcoin::{absolute, sighash, transaction::Version, Sequence, TxIn, Witness};

    let script = ScriptBuf::new_p2tr_tweaked(recipient_pubkey.dangerous_assume_tweaked());
    if txout.value < (fees + Amount::from_sat(DUST)) {
        return None;
    }
    let value = txout.value - fees;
    let output = vec![TxOut {
        value,
        script_pubkey: script,
    }];
    let input = vec![TxIn {
        previous_output: outpoint,
        script_sig: Default::default(),
        sequence: Sequence::ZERO,
        witness: Default::default(),
    }];
    let mut tx = bitcoin::Transaction {
        version: Version::TWO,
        lock_time: absolute::LockTime::ZERO,
        input,
        output,
    };

    let keypair = bitcoin::secp256k1::Keypair::from_secret_key(secp, &sk);
    #[allow(deprecated)]
    let keypair = keypair.tap_tweak(secp, None).to_inner();

    let mut cache = sighash::SighashCache::new(tx.clone());
    let sighash_type = sighash::TapSighashType::Default;
    let txouts = vec![txout.clone()];
    let prevouts = sighash::Prevouts::All(&txouts);
    let sighash = cache
        .taproot_key_spend_signature_hash(0, &prevouts, sighash_type)
        .ok()?;
    let sighash = bitcoin::secp256k1::Message::from_digest_slice(
        &bitcoin::hashes::Hash::to_byte_array(sighash),
    )
    .expect("Sighash is always 32 bytes.");

    let signature = secp.sign_schnorr_no_aux_rand(&sighash, &keypair);
    let sig = bitcoin::taproot::Signature {
        signature,
        sighash_type,
    };

    let witness = Witness::p2tr_key_spend(&sig);
    tx.input[0].witness = witness;

    Some(tx)
}

/// Fund a Silent Payment wallet by creating an SP transaction on regtest.
///
/// Returns the block height at which the SP output was mined.
///
/// Flow:
/// 1. Derive taproot address from mnemonic
/// 2. Fund it via bitcoind
/// 3. Build SP transaction to the wallet's SP address
/// 4. Broadcast and mine
pub fn fund_sp_wallet(
    bitcoind: &mut corepc_node::Client,
    bbd_url: &str,
    mnemonic: &str,
    amount_btc: f64,
) -> u32 {
    let secp = bitcoin::secp256k1::Secp256k1::new();
    let network = bitcoin::Network::Regtest;

    // Create SP client to get the receiving address
    let bip39_mnemonic = bip39::Mnemonic::parse(mnemonic).expect("valid mnemonic");
    let sp_client =
        SpReceiver::new_from_mnemonic(bip39_mnemonic.clone(), network).expect("sp_client");
    let sp_address = sp_client.get_receiving_address();

    // Create taproot signer from same mnemonic
    let tr_signer =
        HotSigner::new_taproot_from_mnemonics(network, mnemonic).expect("taproot signer");
    let tr_derivator = tr_signer
        .descriptors()
        .into_iter()
        .next()
        .expect("descriptor")
        .as_miniscript()
        .expect("miniscript descriptor")
        .spk_derivator(network)
        .expect("derivator");
    let taproot_addr = tr_derivator.receive_at(0);

    // Build derivation path for index 0
    let path = tr_path(
        network,
        ChildNumber::from_hardened_idx(0).expect("child number"),
    )
    .expect("tr_path");
    let path = path.child(ChildNumber::from_normal_idx(0).expect("child number"));
    let path = path.child(ChildNumber::from_normal_idx(0).expect("child number"));
    let sk = tr_signer.private_key_at(&path);

    // Fund the taproot address
    let fund_txid =
        bwk_test::send(bitcoind, taproot_addr.clone(), amount_btc).expect("fund taproot address");
    bwk_test::generate_blocks(bitcoind, 2);

    let current_height = bwk_test::get_height(bitcoind);
    wait_for_sync_and_index(bbd_url, current_height as u32);

    // Get the funded UTXO
    let tx = bwk_test::get_tx(bitcoind, fund_txid).expect("get tx");
    let (index, txout) = bwk_test::txouts_for(&taproot_addr, &tx)
        .into_iter()
        .next()
        .expect("find txout");
    let outpoint = OutPoint {
        txid: fund_txid,
        vout: index as u32,
    };

    // Create SP transaction
    let recipient_pubkey = generate_recipient_pubkey(sk, outpoint, &txout, sp_address, &secp)
        .expect("generate recipient pubkey");
    let sp_tx = swap_to_sp(
        sk,
        outpoint,
        txout,
        recipient_pubkey,
        Amount::from_sat(1000),
        &secp,
    )
    .expect("create sp tx");

    // Broadcast and mine
    let sp_txid = sp_tx.compute_txid();
    bitcoind
        .send_raw_transaction(&sp_tx)
        .expect("broadcast sp tx");
    bwk_test::generate_blocks(bitcoind, 1);

    let sp_height = bwk_test::get_tx_height(bitcoind, sp_txid).expect("get tx height") as u32;
    wait_for_sync_and_index(bbd_url, sp_height);

    sp_height
}

// ===== Electrum Helpers =====

/// Create a BlindbitD instance with electrs enabled.
/// Returns (BlindbitD, bitcoind_node, electrum_url).
pub fn setup_blindbitd_with_electrum() -> (BlindbitD, corepc_node::Node, String) {
    let mut bbd = BlindbitD::new().expect("failed to start BlindbitD");
    let electrum_url = bbd
        .electrsd
        .as_ref()
        .expect("electrsd not available")
        .electrum_url
        .clone();
    let bitcoind_node = bbd.bitcoin().expect("failed to get bitcoind");
    (bbd, bitcoind_node, electrum_url)
}

/// Create a Silent Account with electrum URL configured (enables sub-accounts).
pub fn create_test_account_with_electrum(
    account_name: &str,
    blindbit_url: &str,
    electrum_url: &str,
) -> Account {
    create_test_account_with_electrum_mnemonic(
        account_name,
        blindbit_url,
        electrum_url,
        TEST_MNEMONIC,
    )
}

/// Create a Silent Account with electrum URL and custom mnemonic.
pub fn create_test_account_with_electrum_mnemonic(
    account_name: &str,
    blindbit_url: &str,
    electrum_url: &str,
    mnemonic: &str,
) -> Account {
    let config = Config::new(
        account_name.to_string(),
        Network::Regtest,
        descriptor_from_mnemonic(mnemonic, Network::Regtest),
        blindbit_url.to_string(),
        electrum_url.to_string(),
        Some(546),
    );
    Account::new(config).expect("Account creation should succeed")
}

/// Fund an SP wallet a second time at a different derivation index.
/// Used for tests requiring multiple UTXOs.
pub fn fund_sp_wallet_at_index(
    bitcoind: &mut corepc_node::Client,
    bbd_url: &str,
    mnemonic: &str,
    amount_btc: f64,
    derivation_index: u32,
) -> u32 {
    let secp = bitcoin::secp256k1::Secp256k1::new();
    let network = bitcoin::Network::Regtest;

    let bip39_mnemonic = bip39::Mnemonic::parse(mnemonic).expect("valid mnemonic");
    let sp_client =
        SpReceiver::new_from_mnemonic(bip39_mnemonic.clone(), network).expect("sp_client");
    let sp_address = sp_client.get_receiving_address();

    let tr_signer =
        HotSigner::new_taproot_from_mnemonics(network, mnemonic).expect("taproot signer");
    let tr_derivator = tr_signer
        .descriptors()
        .into_iter()
        .next()
        .expect("descriptor")
        .as_miniscript()
        .expect("miniscript descriptor")
        .spk_derivator(network)
        .expect("derivator");
    let taproot_addr = tr_derivator.receive_at(derivation_index);

    let path = tr_path(
        network,
        ChildNumber::from_hardened_idx(0).expect("child number"),
    )
    .expect("tr_path");
    let path = path.child(ChildNumber::from_normal_idx(0).expect("child number"));
    let path = path.child(ChildNumber::from_normal_idx(derivation_index).expect("child number"));
    let sk = tr_signer.private_key_at(&path);

    let fund_txid =
        bwk_test::send(bitcoind, taproot_addr.clone(), amount_btc).expect("fund taproot address");
    bwk_test::generate_blocks(bitcoind, 2);

    let current_height = bwk_test::get_height(bitcoind);
    wait_for_sync_and_index(bbd_url, current_height as u32);

    let tx = bwk_test::get_tx(bitcoind, fund_txid).expect("get tx");
    let (index, txout) = bwk_test::txouts_for(&taproot_addr, &tx)
        .into_iter()
        .next()
        .expect("find txout");
    let outpoint = OutPoint {
        txid: fund_txid,
        vout: index as u32,
    };

    let recipient_pubkey = generate_recipient_pubkey(sk, outpoint, &txout, sp_address, &secp)
        .expect("generate recipient pubkey");
    let sp_tx = swap_to_sp(
        sk,
        outpoint,
        txout,
        recipient_pubkey,
        Amount::from_sat(1000),
        &secp,
    )
    .expect("create sp tx");

    let sp_txid = sp_tx.compute_txid();
    bitcoind
        .send_raw_transaction(&sp_tx)
        .expect("broadcast sp tx");
    bwk_test::generate_blocks(bitcoind, 1);

    let sp_height = bwk_test::get_tx_height(bitcoind, sp_txid).expect("get tx height") as u32;
    wait_for_sync_and_index(bbd_url, sp_height);

    sp_height
}
