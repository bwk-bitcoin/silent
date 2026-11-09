//! Backups of a built-in account (ws-plaude/silent#91): its BIP139 wallet backup document,
//! encrypted with BIP138 to a Multikey account's policy keys, any one of which opens it, or
//! to an SP account's scan secret.
//!
//! The document holds the account's descriptors, the receive and change indexes each one
//! handed out, the block SP scanning starts from and every label, and in its
//! `proprietary` object the servers and settings only Silent reads. It holds no signer
//! secret: the keys stay in Keys. Coins and history are not in it either: a restored
//! account finds them again by scanning. A Multikey account's saved PSBTs and signing
//! sessions are left out, so a restore never brings back nonces a signer already used.
//!
//! A restore reads, from the file's contents and in this order, BIP138(BIP139),
//! BIP138(descriptor), a plain BIP139 document, then a plain descriptor, and restores
//! what each carries: a descriptor alone restores the account scanning from the
//! network's default start.

use std::str::FromStr;

use bip138::{descriptor::dpk_to_root_pk, Content};
use bitcoin::{
    bip32::{ChildNumber, DerivationPath},
    secp256k1::{PublicKey, Secp256k1},
    NetworkKind,
};
use bwk::{
    bwk_descriptor::musig::parse_musig_descriptor, bwk_electrum::scanner::ElectrumScanner,
    bwk_keys::musig::MusigKey,
};
use bwk_sp::{
    bwk_sign::{
        bwk_descriptor::sp_descriptor::SpDescriptor,
        hot_signer::HotSigner,
        miniscript::{Descriptor, DescriptorPublicKey, ForEachKey},
    },
    receiver::scan_path,
};
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

use crate::{
    app_state::app_last_endpoint,
    backup::{encrypt_document, Encrypted, Item, Recipients},
    bip139::{self, WalletBackup},
    config::{Config, DEFAULT_DUST_LIMIT},
    ffi::{
        AccountBackup, BackendServer, BackupContents, BackupFailure, BackupRestore, BackupTip,
        LabelEditResult, LabelEntry, LabelKind, Network,
    },
    musig::MUSIG_PLUGIN_ID,
    services::{SEGWIT_KIND, TAPROOT_KIND},
};

const SP_PLUGIN_ID: &str = "sp";
/// The key of Silent's own settings in a document's `proprietary` object.
const PROPRIETARY_KEY: &str = "silent";

/// The BIP329 label types Silent keeps, by the kind of what they label.
const BIP329_ADDRESS: &str = "addr";
const BIP329_OUTPUT: &str = "output";
const BIP329_TRANSACTION: &str = "tx";

/// The receive and change indexes the descriptor of one receive kind handed out.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Tip {
    /// The receive kind of its addresses (`SEGWIT_KIND`, `TAPROOT_KIND`).
    pub kind: String,
    pub receive: u32,
    pub change: u32,
}

/// A label, its kind the BIP329 type.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Label {
    pub kind: String,
    pub id: String,
    pub text: String,
}

/// The settings only Silent reads, in the document's `proprietary` object.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
struct Settings {
    #[serde(default)]
    plugin_id: String,
    #[serde(default)]
    blindbit_url: String,
    #[serde(default)]
    electrum_url: String,
    #[serde(default)]
    verify_certificate: Option<bool>,
    #[serde(default)]
    dust_limit: Option<u64>,
}

/// Why a backup was not made, read or restored.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Failure {
    /// None of the formats a restore reads.
    Unknown(String),
    /// A backup in one of them, damaged.
    Malformed(String),
    /// Encrypted, and none of the keys given opens it: the paths of the keys that may.
    Locked(Vec<DerivationPath>),
    InvalidXpub(String),
    InvalidMnemonic,
    /// Decrypted, but it holds neither a BIP139 document nor a descriptor.
    UnsupportedContent,
    /// Its descriptor is none a Silent account takes.
    InvalidDescriptor(String),
    /// For another kind of account, or on a network Silent has not.
    Unsupported(String),
    /// An account of that name exists: a restore never replaces one.
    Exists(String),
    Other(String),
}

impl Failure {
    fn kind(&self) -> BackupFailure {
        match self {
            Self::Unknown(_) => BackupFailure::Unknown,
            Self::Malformed(_) => BackupFailure::Malformed,
            Self::Locked(_) => BackupFailure::Locked,
            Self::InvalidXpub(_) => BackupFailure::InvalidXpub,
            Self::InvalidMnemonic => BackupFailure::InvalidMnemonic,
            Self::UnsupportedContent => BackupFailure::UnsupportedContent,
            Self::InvalidDescriptor(_) => BackupFailure::InvalidDescriptor,
            Self::Unsupported(_) => BackupFailure::Unsupported,
            Self::Exists(_) => BackupFailure::Exists,
            Self::Other(_) => BackupFailure::Other,
        }
    }

    fn message(&self) -> String {
        match self {
            Self::Unknown(error) => format!(
                "Not a backup Silent reads ({error}): expected a BIP138 encrypted backup of a \
                 BIP139 wallet backup or of a descriptor, a BIP139 wallet backup or a descriptor"
            ),
            Self::Malformed(error) => format!("A damaged backup: {error}"),
            Self::Locked(_) => "None of the keys given opens this backup".into(),
            Self::InvalidXpub(xpub) => format!("Invalid xpub: {xpub}"),
            Self::InvalidMnemonic => "Invalid mnemonic".into(),
            Self::UnsupportedContent => {
                "The backup is decrypted, but it holds neither a wallet backup nor a descriptor"
                    .into()
            }
            Self::InvalidDescriptor(error) => format!("Not a descriptor Silent takes: {error}"),
            Self::Unsupported(error) | Self::Exists(error) | Self::Other(error) => error.clone(),
        }
    }

    /// The paths of a locked backup, written m/...
    fn derivation_paths(&self) -> Vec<String> {
        match self {
            Self::Locked(paths) => paths.iter().map(path_text).collect(),
            _ => vec![],
        }
    }
}

fn path_text(path: &DerivationPath) -> String {
    let text = path.to_string();
    if text.starts_with('m') {
        text
    } else {
        format!("m/{text}")
    }
}

// ===== Labels and tips =====

/// The label kind of a BIP329 type, None for a type Silent does not keep.
fn label_kind(bip329: &str) -> Option<LabelKind> {
    match bip329 {
        BIP329_ADDRESS => Some(LabelKind::Address),
        BIP329_OUTPUT => Some(LabelKind::Outpoint),
        BIP329_TRANSACTION => Some(LabelKind::Transaction),
        _ => None,
    }
}

fn bip329_type(kind: LabelKind) -> &'static str {
    match kind {
        LabelKind::Address => BIP329_ADDRESS,
        LabelKind::Outpoint => BIP329_OUTPUT,
        _ => BIP329_TRANSACTION,
    }
}

pub(crate) fn labels_of(entries: Vec<LabelEntry>) -> Vec<Label> {
    entries
        .into_iter()
        .map(|entry| Label {
            kind: bip329_type(entry.kind).into(),
            id: entry.id,
            text: entry.text,
        })
        .collect()
}

/// The receive kind of a descriptor's addresses, by its script.
fn tip_kind(descriptor: &str) -> String {
    match Descriptor::<DescriptorPublicKey>::from_str(descriptor) {
        Ok(Descriptor::Wpkh(_)) => SEGWIT_KIND.into(),
        _ => TAPROOT_KIND.into(),
    }
}

/// The indexes `scanner` handed out.
pub(crate) fn scanner_tip(scanner: &ElectrumScanner) -> Tip {
    let (receive, change) = scanner.generated_addresses();
    Tip {
        kind: tip_kind(&scanner.descriptor().to_string()),
        receive: receive.last().map_or(0, |entry| entry.index),
        change: change.last().map_or(0, |entry| entry.index),
    }
}

/// Moves `scanner` to the indexes of the tip in `tips` of its receive kind, and saves
/// them.
pub(crate) fn restore_scanner_tip(scanner: &ElectrumScanner, tips: &[Tip]) -> Result<(), String> {
    let kind = tip_kind(&scanner.descriptor().to_string());
    let Some(tip) = tips.iter().find(|tip| tip.kind == kind) else {
        return Ok(());
    };
    let mut coins = scanner.coin_store().lock().map_err(|e| e.to_string())?;
    let persisted = {
        let addresses = coins.address_store();
        let mut addresses = addresses.lock().map_err(|e| e.to_string())?;
        addresses.update_recv(tip.receive);
        addresses.update_change(tip.change);
        addresses.try_persist()
    };
    // The coin view follows the new address entries.
    coins.generate();
    persisted.map_err(|e| e.to_string())
}

/// Puts `labels` in an account through `set`, which saves each one.
pub(crate) fn restore_labels(
    labels: &[Label],
    set: impl Fn(LabelKind, String, String) -> LabelEditResult,
) -> Result<(), String> {
    for label in labels {
        let Some(kind) = label_kind(&label.kind) else {
            return Err(format!("Unknown label kind '{}'", label.kind));
        };
        let result = set(kind, label.id.clone(), label.text.clone());
        if !result.is_ok {
            return Err(result.error);
        }
    }
    Ok(())
}

// ===== Networks =====

fn network_name(network: Network) -> &'static str {
    match network {
        Network::Bitcoin => "bitcoin",
        Network::Testnet => "testnet3",
        Network::Signet => "signet",
        _ => "regtest",
    }
}

fn network_of(name: &str) -> Result<Network, Failure> {
    match name {
        "bitcoin" => Ok(Network::Bitcoin),
        "testnet3" => Ok(Network::Testnet),
        "signet" => Ok(Network::Signet),
        "regtest" => Ok(Network::Regtest),
        other => Err(Failure::Unsupported(format!(
            "This backup is of the '{other}' network, which Silent has not"
        ))),
    }
}

/// The network a descriptor's keys are of, when they tell it: only mainnet keys name
/// their network, test keys are the same on every test network.
fn network_of_keys(descriptor: &str) -> Option<Network> {
    let kind = if let Ok(sp) = SpDescriptor::from_str(descriptor) {
        sp.network_kind()
    } else {
        let parsed = parse_musig_descriptor(descriptor).ok()?;
        let mut kind = None;
        parsed.for_each_key(|key| {
            let keys = match key {
                MusigKey::Plain(k) => vec![k],
                MusigKey::Musig(aggregate) => aggregate.cosigners().iter().collect(),
            };
            for key in keys {
                if let DescriptorPublicKey::XPub(xpub) = key {
                    kind = Some(xpub.xkey.network);
                }
            }
            true
        });
        kind?
    };
    (kind == NetworkKind::Main).then_some(Network::Bitcoin)
}

// ===== Backup =====

/// What the backup of an account is encrypted to: the 32-byte BIP138 keys, the paths a
/// restore asks for them at, and the key expressions left out of them.
struct Keying {
    keys: Zeroizing<Vec<[u8; 32]>>,
    paths: Vec<DerivationPath>,
    excluded: Vec<String>,
}

/// A Multikey account's backup is encrypted to its policy's keys, as BIP138 picks them.
fn policy_keying(descriptor: &str) -> Result<Keying, String> {
    let mut recipients = Recipients::default();
    recipients.add_descriptor(descriptor)?;
    recipients.drop_exposed();
    Ok(Keying {
        keys: Zeroizing::new(recipients.bip138_keys()),
        paths: recipients.derivation_paths(),
        excluded: recipients.excluded().to_vec(),
    })
}

/// An SP account's backup is encrypted to its scan secret alone, at the BIP352 scan path.
/// The backup carries the scan secret (in the sp() descriptor) and is keyed by it: a
/// knowing departure from BIP138, which asks to remove private material and to key by
/// public keys. Unlike the spend key, which the SP address shows, and the sub-accounts'
/// xpubs, shared more widely, the scan secret only opens it for whoever can already see
/// the account's payments.
fn scan_keying(config: &Config) -> Result<Keying, String> {
    let sp = SpDescriptor::from_str(&config.descriptor).map_err(|e| e.to_string())?;
    let scan = sp
        .scan_secret_key(&Secp256k1::new())
        .map_err(|e| e.to_string())?;
    let paths = sp
        .origin()
        .and_then(|(_, origin)| origin.into_iter().nth(2).copied())
        .map(|account| scan_path(config.network.into(), account))
        .into_iter()
        .collect();
    Ok(Keying {
        keys: Zeroizing::new(vec![scan.secret_bytes()]),
        paths,
        excluded: vec![],
    })
}

fn keying(config: &Config) -> Result<Keying, String> {
    if config.plugin_id == MUSIG_PLUGIN_ID {
        policy_keying(&config.descriptor)
    } else {
        scan_keying(config)
    }
}

fn settings_of(config: &Config) -> Settings {
    Settings {
        plugin_id: config.plugin_id.clone(),
        blindbit_url: config.blindbit_url.clone(),
        electrum_url: config.electrum_url.clone(),
        verify_certificate: Some(config.verify_certificate),
        dust_limit: config.dust_limit,
    }
}

/// An account of the document holding a BIP380 descriptor, with the indexes of its tip.
fn bip380_account(descriptor: &str, tip: Option<&Tip>) -> bip139::Account {
    bip139::Account {
        kind: Some(bip139::TYPE_BIP380.into()),
        descriptor: Some(serde_json::Value::String(descriptor.into())),
        receive_index: tip.map(|tip| tip.receive),
        change_index: tip.map(|tip| tip.change),
        ..Default::default()
    }
}

/// The BIP139 document of the account `config` describes: an SP account first, its
/// sub-accounts after it, or a Multikey account's policy. Every label goes on the first.
fn document(
    config: &Config,
    scan_start: Option<u32>,
    tips: &[Tip],
    labels: Vec<Label>,
) -> Result<WalletBackup, String> {
    let tip = |kind: &str| tips.iter().find(|tip| tip.kind == kind);
    let mut accounts = if config.plugin_id == MUSIG_PLUGIN_ID {
        vec![bip380_account(&config.descriptor, tip(TAPROOT_KIND))]
    } else {
        let mut accounts = vec![bip139::Account {
            kind: Some(bip139::TYPE_BIP392.into()),
            descriptor: Some(serde_json::Value::String(config.descriptor.clone())),
            block_height: scan_start,
            ..Default::default()
        }];
        for (descriptor, kind) in [
            (&config.segwit_descriptor, SEGWIT_KIND),
            (&config.taproot_descriptor, TAPROOT_KIND),
        ] {
            if !descriptor.is_empty() {
                accounts.push(bip380_account(descriptor, tip(kind)));
            }
        }
        accounts
    };
    if !labels.is_empty() {
        accounts[0].bip329_labels = Some(
            labels
                .into_iter()
                .map(|label| bip139::Label {
                    kind: label.kind,
                    reference: label.id,
                    label: Some(label.text),
                })
                .collect(),
        );
    }
    let settings = serde_json::to_value(settings_of(config)).map_err(|e| e.to_string())?;
    Ok(WalletBackup {
        version: Some(bip139::VERSION),
        bip: Some(bip139::BIP),
        name: Some(config.account_name.clone()),
        accounts,
        network: Some(network_name(config.network).into()),
        proprietary: Some(serde_json::json!({ PROPRIETARY_KEY: settings })),
    })
}

/// The backup of the account `config` describes, with the state the account holds, and
/// the key expressions it is not encrypted to, whose holders cannot open it with them.
pub(crate) fn backup(
    config: &Config,
    scan_start: Option<u32>,
    tips: Vec<Tip>,
    labels: Vec<Label>,
) -> Result<(Vec<u8>, Vec<String>), String> {
    let document = document(config, scan_start, &tips, labels)?;
    let json = Zeroizing::new(serde_json::to_vec(&document).map_err(|e| e.to_string())?);
    let keying = keying(config)?;
    let bytes = encrypt_document(&json, keying.keys.to_vec(), &keying.paths)?;
    Ok((bytes, keying.excluded))
}

pub(crate) fn backup_result(result: Result<(Vec<u8>, Vec<String>), String>) -> AccountBackup {
    match result {
        Ok((bytes, excluded)) => AccountBackup {
            is_ok: true,
            error: String::new(),
            bytes,
            excluded,
        },
        Err(error) => AccountBackup {
            is_ok: false,
            error,
            bytes: vec![],
            excluded: vec![],
        },
    }
}

// ===== Reading =====

/// What a backup file holds, once decrypted.
#[derive(Debug, Clone)]
enum Plaintext {
    Document(WalletBackup),
    Descriptor(String),
}

/// Whether `text` is a descriptor a Silent account takes: an sp() one, or a policy.
fn is_descriptor(text: &str) -> bool {
    SpDescriptor::from_str(text).is_ok() || parse_musig_descriptor(text).is_ok()
}

fn xonly(key: &PublicKey) -> [u8; 32] {
    key.x_only_public_key().0.serialize()
}

/// The BIP138 key `key` gives: the scan secret of an sp() descriptor, a key's answer when
/// asked for its silent payment account, or the root of an xpub, with or without origin
/// and derivation.
fn given_key(key: &str) -> Result<[u8; 32], Failure> {
    if let Ok(sp) = SpDescriptor::from_str(key.trim()) {
        return sp
            .scan_secret_key(&Secp256k1::new())
            .map(|scan| scan.secret_bytes())
            .map_err(|e| Failure::InvalidDescriptor(e.to_string()));
    }
    DescriptorPublicKey::from_str(key.trim())
        .ok()
        .and_then(|key| dpk_to_root_pk(&key).ok())
        .map(|key| xonly(&key))
        .ok_or_else(|| Failure::InvalidXpub(key.into()))
}

/// Whether `path` is the BIP352 scan path of an account, `352'/coin'/account'/1'/0`.
fn is_scan_path(path: &DerivationPath) -> bool {
    let steps: Vec<ChildNumber> = path.into_iter().copied().collect();
    let [purpose, coin, account, _, _] = steps.as_slice() else {
        return false;
    };
    let network = match coin {
        ChildNumber::Hardened { index: 0 } => bitcoin::Network::Bitcoin,
        _ => bitcoin::Network::Testnet,
    };
    *purpose == ChildNumber::Hardened { index: 352 }
        && coin.is_hardened()
        && scan_path(network, *account) == *path
}

/// The BIP138 keys of `mnemonic` at `paths`, none for an empty mnemonic: its scan secret
/// at a BIP352 scan path, its x-only public key elsewhere.
fn mnemonic_keys(
    mnemonic: &str,
    paths: &[DerivationPath],
) -> Result<Zeroizing<Vec<[u8; 32]>>, Failure> {
    if mnemonic.trim().is_empty() {
        return Ok(Zeroizing::new(vec![]));
    }
    // A key does not depend on the network the signer is made for.
    let signer = HotSigner::new_from_mnemonics(bitcoin::Network::Bitcoin, mnemonic.trim())
        .map_err(|_| Failure::InvalidMnemonic)?;
    Ok(Zeroizing::new(
        paths
            .iter()
            .map(|path| {
                if is_scan_path(path) {
                    signer.private_key_at(path).secret_bytes()
                } else {
                    xonly(&signer.public_key_at(path))
                }
            })
            .collect(),
    ))
}

/// The BIP139 document or the descriptor the decrypted `items` hold, in that order.
fn decrypted(items: Vec<Item>) -> Result<Plaintext, Failure> {
    if let Some((_, bytes)) = items
        .iter()
        .find(|(content, _)| *content == Content::Bip139)
    {
        return WalletBackup::parse(bytes)
            .map(Plaintext::Document)
            .map_err(Failure::Malformed);
    }
    let Some((_, bytes)) = items
        .iter()
        .find(|(content, _)| *content == Content::Bip380)
    else {
        return Err(Failure::UnsupportedContent);
    };
    let text = std::str::from_utf8(bytes)
        .map_err(|e| Failure::InvalidDescriptor(e.to_string()))?
        .trim();
    if is_descriptor(text) {
        Ok(Plaintext::Descriptor(text.into()))
    } else {
        Err(Failure::InvalidDescriptor(text.into()))
    }
}

fn open_encrypted(
    encrypted: &Encrypted,
    keys: &[String],
    mnemonic: &str,
) -> Result<Plaintext, Failure> {
    let paths = encrypted.lookup_paths();
    let mut keys = Zeroizing::new(
        keys.iter()
            .map(|key| given_key(key))
            .collect::<Result<Vec<_>, _>>()?,
    );
    keys.extend(mnemonic_keys(mnemonic, &paths)?.iter());
    match encrypted.open(&keys).map_err(Failure::Malformed)? {
        Some(items) => decrypted(items),
        None => Err(Failure::Locked(paths)),
    }
}

/// A plain BIP139 document, or a plain descriptor.
fn read_plain(bytes: &[u8]) -> Result<Plaintext, Failure> {
    let text = std::str::from_utf8(bytes)
        .map_err(|_| Failure::Unknown("not text".into()))?
        .trim();
    if text.starts_with('{') {
        return WalletBackup::parse(text.as_bytes())
            .map(Plaintext::Document)
            .map_err(Failure::Unknown);
    }
    if is_descriptor(text) {
        Ok(Plaintext::Descriptor(text.into()))
    } else {
        Err(Failure::Unknown(
            "neither a wallet backup nor a descriptor".into(),
        ))
    }
}

/// What the backup `bytes` holds, an encrypted one opened with `keys` or `mnemonic`.
fn open(bytes: &[u8], keys: &[String], mnemonic: &str) -> Result<Plaintext, Failure> {
    match Encrypted::read(bytes) {
        Some(encrypted) => open_encrypted(&encrypted.map_err(Failure::Malformed)?, keys, mnemonic),
        None => read_plain(bytes),
    }
}

// ===== Restorable =====

/// What a backup restores: the account and the state put in it.
#[derive(Debug, Clone, Default)]
struct Restorable {
    plugin_id: String,
    /// Empty when the backup names no account.
    name: String,
    /// None when neither the backup nor its keys tell the network.
    network: Option<Network>,
    descriptor: String,
    segwit_descriptor: String,
    taproot_descriptor: String,
    settings: Settings,
    scan_start: Option<u32>,
    tips: Vec<Tip>,
    labels: Vec<Label>,
}

/// The kind of account `descriptor` is of.
fn plugin_of(descriptor: &str) -> &'static str {
    if SpDescriptor::from_str(descriptor).is_ok() {
        SP_PLUGIN_ID
    } else {
        MUSIG_PLUGIN_ID
    }
}

/// The descriptor of each account of a document.
fn descriptors(document: &WalletBackup) -> Result<Vec<&str>, Failure> {
    document
        .accounts
        .iter()
        .map(|account| {
            account
                .descriptor_text()
                .map_err(Failure::Unsupported)?
                .ok_or_else(|| {
                    Failure::Unsupported("An account of the backup has no descriptor".into())
                })
        })
        .collect()
}

/// The tip an account of a document names, None when it names no index.
fn account_tip(account: &bip139::Account, descriptor: &str) -> Option<Tip> {
    if account.receive_index.is_none() && account.change_index.is_none() {
        return None;
    }
    Some(Tip {
        kind: tip_kind(descriptor),
        receive: account.receive_index.unwrap_or(0),
        change: account.change_index.unwrap_or(0),
    })
}

/// An SP account and its sub-accounts: one sp() account, at most one segwit and one
/// taproot account beside it.
fn sp_accounts(document: &WalletBackup, restorable: &mut Restorable) -> Result<(), Failure> {
    for (account, descriptor) in document.accounts.iter().zip(descriptors(document)?) {
        if SpDescriptor::from_str(descriptor).is_ok() {
            if !restorable.descriptor.is_empty() {
                return Err(Failure::Unsupported(
                    "This backup holds more than one silent payment account".into(),
                ));
            }
            restorable.descriptor = descriptor.into();
            restorable.scan_start = account.block_height;
            continue;
        }
        let slot = match Descriptor::<DescriptorPublicKey>::from_str(descriptor) {
            Ok(Descriptor::Wpkh(_)) => &mut restorable.segwit_descriptor,
            Ok(Descriptor::Tr(_)) => &mut restorable.taproot_descriptor,
            _ => {
                return Err(Failure::InvalidDescriptor(format!(
                    "{descriptor}: not a sub-account of a silent payment account"
                )))
            }
        };
        if !slot.is_empty() {
            return Err(Failure::Unsupported(
                "This backup holds two sub-accounts of the same kind".into(),
            ));
        }
        *slot = descriptor.into();
        restorable.tips.extend(account_tip(account, descriptor));
    }
    Ok(())
}

/// A Multikey account: one policy account.
fn multikey_account(document: &WalletBackup, restorable: &mut Restorable) -> Result<(), Failure> {
    let descriptors = descriptors(document)?;
    let ([account], [descriptor]) = (document.accounts.as_slice(), descriptors.as_slice()) else {
        return Err(Failure::Unsupported(
            "A Multikey backup holds one account".into(),
        ));
    };
    parse_musig_descriptor(descriptor).map_err(|e| Failure::InvalidDescriptor(e.to_string()))?;
    restorable.descriptor = (*descriptor).into();
    restorable.tips.extend(account_tip(account, descriptor));
    Ok(())
}

fn from_document(document: WalletBackup) -> Result<Restorable, Failure> {
    let settings = match document
        .proprietary
        .as_ref()
        .and_then(|proprietary| proprietary.get(PROPRIETARY_KEY))
    {
        Some(settings) => serde_json::from_value::<Settings>(settings.clone())
            .map_err(|e| Failure::Malformed(e.to_string()))?,
        None => Settings::default(),
    };
    let plugin_id = if settings.plugin_id.is_empty() {
        let any_sp = descriptors(&document)?
            .iter()
            .any(|descriptor| plugin_of(descriptor) == SP_PLUGIN_ID);
        if any_sp {
            SP_PLUGIN_ID
        } else {
            MUSIG_PLUGIN_ID
        }
        .to_string()
    } else {
        settings.plugin_id.clone()
    };
    let mut restorable = Restorable {
        plugin_id,
        name: document
            .name
            .clone()
            .or_else(|| document.accounts[0].name.clone())
            .unwrap_or_default(),
        settings,
        ..Default::default()
    };
    match restorable.plugin_id.as_str() {
        SP_PLUGIN_ID => sp_accounts(&document, &mut restorable)?,
        MUSIG_PLUGIN_ID => multikey_account(&document, &mut restorable)?,
        other => {
            return Err(Failure::Unsupported(format!(
                "This backup is of a '{other}' account, which Silent has not"
            )))
        }
    }
    if restorable.descriptor.is_empty() {
        return Err(Failure::Unsupported(
            "This backup holds no silent payment account".into(),
        ));
    }
    restorable.network = match &document.network {
        Some(name) => Some(network_of(name)?),
        None => network_of_keys(&restorable.descriptor),
    };
    // BIP139 asks to keep what is supported: labels of other BIP329 types are dropped.
    for account in &document.accounts {
        for label in account.bip329_labels.iter().flatten() {
            if let (Some(_), Some(text)) = (label_kind(&label.kind), &label.label) {
                restorable.labels.push(Label {
                    kind: label.kind.clone(),
                    id: label.reference.clone(),
                    text: text.clone(),
                });
            }
        }
    }
    Ok(restorable)
}

fn from_descriptor(descriptor: String) -> Restorable {
    Restorable {
        plugin_id: plugin_of(&descriptor).into(),
        network: network_of_keys(&descriptor),
        descriptor,
        ..Default::default()
    }
}

fn restorable(plaintext: Plaintext) -> Result<Restorable, Failure> {
    match plaintext {
        Plaintext::Document(document) => from_document(document),
        Plaintext::Descriptor(descriptor) => Ok(from_descriptor(descriptor)),
    }
}

impl Restorable {
    /// The account's config, named `name` on `network`.
    fn config(&self, name: String, network: Network) -> Config {
        let mut config = Config::new(
            name,
            network,
            self.descriptor.clone(),
            self.settings.blindbit_url.clone(),
            self.settings.electrum_url.clone(),
            self.settings.dust_limit.or(Some(DEFAULT_DUST_LIMIT)),
        );
        config.segwit_descriptor = self.segwit_descriptor.clone();
        config.taproot_descriptor = self.taproot_descriptor.clone();
        config.verify_certificate = self.settings.verify_certificate.unwrap_or(true);
        config.plugin_id = self.plugin_id.clone();
        if self.plugin_id == SP_PLUGIN_ID {
            config.scan_start = self.scan_start;
        }
        config
    }
}

// ===== Inspection =====

/// The fingerprints of the keys that sign for the account, sorted.
fn signers(config: &Config) -> Vec<String> {
    let mut fingerprints = std::collections::BTreeSet::new();
    if config.plugin_id == MUSIG_PLUGIN_ID {
        fingerprints.extend(crate::musig::musig_descriptor_fingerprints(
            config.descriptor.clone(),
        ));
    } else if let Ok(SpDescriptor::Packed {
        origin: Some((fingerprint, _)),
        ..
    }) = SpDescriptor::from_str(&config.descriptor)
    {
        fingerprints.insert(fingerprint.to_string());
    }
    for sub_account in [&config.segwit_descriptor, &config.taproot_descriptor] {
        let Ok(descriptor) = Descriptor::<DescriptorPublicKey>::from_str(sub_account) else {
            continue;
        };
        descriptor.for_each_key(|key| {
            fingerprints.insert(key.master_fingerprint().to_string());
            true
        });
    }
    fingerprints.into_iter().collect()
}

fn contents(restorable: &Restorable) -> BackupContents {
    let network = restorable.network.unwrap_or(Network::Regtest);
    let config = restorable.config(restorable.name.clone(), network);
    let mut endpoints = vec![];
    for url in [&config.blindbit_url, &config.electrum_url] {
        if !url.is_empty() {
            endpoints.push(url.clone());
        }
    }
    BackupContents {
        is_ok: true,
        error: String::new(),
        failure: BackupFailure::None,
        plugin_id: config.plugin_id.clone(),
        account: config.account_name.clone(),
        network,
        network_known: restorable.network.is_some(),
        endpoints,
        tips: restorable
            .tips
            .iter()
            .map(|tip| BackupTip {
                kind: tip.kind.clone(),
                receive: tip.receive,
                change: tip.change,
            })
            .collect(),
        scan_start: restorable.scan_start.unwrap_or(0),
        labels: restorable.labels.len() as u32,
        signers: signers(&config),
        saved_psbts_excluded: config.plugin_id == MUSIG_PLUGIN_ID,
        derivation_paths: vec![],
    }
}

fn contents_failed(failure: Failure) -> BackupContents {
    BackupContents {
        is_ok: false,
        error: failure.message(),
        failure: failure.kind(),
        plugin_id: String::new(),
        account: String::new(),
        network: Network::Regtest,
        network_known: false,
        endpoints: vec![],
        tips: vec![],
        scan_start: 0,
        labels: 0,
        signers: vec![],
        saved_psbts_excluded: false,
        derivation_paths: failure.derivation_paths(),
    }
}

/// What the backup `bytes` holds, an encrypted one opened with one of `keys` (xpubs, or
/// the sp() descriptor a key gives) or `mnemonic`, and
/// what restoring it needs. Refused as `Locked` when none of them opens it.
pub fn inspect_account_backup(bytes: &[u8], keys: Vec<String>, mnemonic: String) -> BackupContents {
    let mnemonic = Zeroizing::new(mnemonic);
    match open(bytes, &keys, &mnemonic).and_then(restorable) {
        Ok(restorable) => contents(&restorable),
        Err(failure) => contents_failed(failure),
    }
}

// ===== Restore =====

/// Opens the restored account offline, puts the backup's indexes and labels in it,
/// then closes it, flushing its stores.
fn apply_state(config: &Config, tips: &[Tip], labels: &[Label]) -> Result<(), String> {
    if config.plugin_id == MUSIG_PLUGIN_ID {
        let mut account = crate::musig::new_musig_account_offline(config);
        if !account.is_ok() {
            return Err(account.get_error());
        }
        let applied = account.restore_state(tips, labels);
        let flushed = account.shutdown_backend();
        applied?;
        return if flushed.is_empty() {
            Ok(())
        } else {
            Err(flushed)
        };
    }
    let mut account = crate::account::new_account_offline(config);
    if !account.is_ok() {
        return Err(account.get_error());
    }
    let applied = account.restore_state(tips, labels);
    let flushed = account.shutdown_backend();
    applied?;
    if flushed.is_empty() {
        Ok(())
    } else {
        Err(flushed)
    }
}

/// The servers a backup names, else the ones the last account of `network` used, as
/// account creation offers them.
fn fill_endpoints(config: &mut Config) -> Result<(), Failure> {
    if config.blindbit_url.is_empty() && config.plugin_id == SP_PLUGIN_ID {
        config.blindbit_url = app_last_endpoint(BackendServer::Blindbit, config.network);
        if config.blindbit_url.is_empty() {
            return Err(Failure::Other(format!(
                "This backup names no BlindBit server and no account on {} used one yet",
                network_name(config.network)
            )));
        }
    }
    if config.electrum_url.is_empty() {
        config.electrum_url = app_last_endpoint(BackendServer::Electrum, config.network);
    }
    Ok(())
}

fn restore(
    plugin_id: &str,
    bytes: &[u8],
    keys: &[String],
    mnemonic: &str,
    name: String,
    network: Network,
) -> Result<String, Failure> {
    let restorable = restorable(open(bytes, keys, mnemonic)?)?;
    if restorable.plugin_id != plugin_id {
        return Err(Failure::Unsupported(format!(
            "This backup is of a '{}' account, not a '{plugin_id}' one",
            restorable.plugin_id
        )));
    }
    if ![SP_PLUGIN_ID, MUSIG_PLUGIN_ID].contains(&plugin_id) {
        return Err(Failure::Unsupported(format!(
            "No '{plugin_id}' accounts here"
        )));
    }
    let mut config = restorable.config(name, restorable.network.unwrap_or(network));
    fill_endpoints(&mut config)?;
    if plugin_id == SP_PLUGIN_ID {
        config.to_sp_config().map_err(Failure::InvalidDescriptor)?;
    }
    let created = crate::config::persist_creation(config.clone());
    if !created.is_ok {
        return Err(if created.error == "already_exists" {
            Failure::Exists(format!(
                "An account named '{}' exists: delete it first",
                config.account_name
            ))
        } else {
            Failure::Other(created.error)
        });
    }
    if let Err(error) = apply_state(&config, &restorable.tips, &restorable.labels) {
        // Nothing partial stays: the account created goes.
        if let Err(removal) = crate::account_storage::delete_owned(plugin_id, &config.account_name)
        {
            log::error!("restore: removing the partial account failed: {removal}");
        }
        return Err(Failure::Other(error));
    }
    Ok(config.account_name.clone())
}

/// Restores the `plugin_id` account the backup `bytes` holds, an encrypted one opened
/// with `keys` or `mnemonic`, as the new account `name`: refused when one exists. On
/// `network` when the backup does not tell its own.
pub fn restore_account_backup(
    plugin_id: &str,
    bytes: &[u8],
    keys: Vec<String>,
    mnemonic: String,
    name: String,
    network: Network,
) -> BackupRestore {
    let mnemonic = Zeroizing::new(mnemonic);
    match restore(plugin_id, bytes, &keys, &mnemonic, name, network) {
        Ok(account) => BackupRestore {
            is_ok: true,
            error: String::new(),
            failure: BackupFailure::None,
            account,
        },
        Err(failure) => BackupRestore {
            is_ok: false,
            error: failure.message(),
            failure: failure.kind(),
            account: String::new(),
        },
    }
}
