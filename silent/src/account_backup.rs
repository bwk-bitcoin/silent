//! Backups of a built-in account (ws-plaude/silent#78): what a restore needs to rebuild
//! it, sealed under a password.
//!
//! A backup holds the account's config (its descriptors and endpoints), the receive and
//! change indexes each descriptor handed out, the block SP scanning starts from and
//! every label. It holds no signer secret: the keys stay in Keys, which a restore needs.
//! Coins and history are not in it either: a restored account finds them again by
//! scanning. A Multikey account's saved PSBTs and signing sessions are left out, so a
//! restore never brings back nonces a signer already used.
//!
//! The file is `MAGIC` then the JSON of the sealed envelope: Argon2id derives the key
//! from the password and XChaCha20-Poly1305 seals the payload (`keys::crypto`), bound to
//! its use by `AAD`.

use std::str::FromStr;

use bwk::bwk_electrum::scanner::ElectrumScanner;
use bwk_sp::bwk_sign::{
    bwk_descriptor::sp_descriptor::SpDescriptor,
    miniscript::{Descriptor, DescriptorPublicKey, ForEachKey},
};
use serde::{Deserialize, Serialize};
use zeroize::Zeroizing;

use crate::{
    config::{datadir, Config},
    ffi::{
        AccountBackup, BackupContents, BackupFailure, BackupRestore, BackupTip, LabelEditResult,
        LabelEntry, LabelKind, Network,
    },
    keys::{
        crypto::{open_with_aad, seal_with_aad, Password},
        error::Error as KeyError,
        record::SecretEnvelope,
    },
    musig::MUSIG_PLUGIN_ID,
    services::{SEGWIT_KIND, TAPROOT_KIND},
};

/// What a backup file starts with.
pub const MAGIC: &[u8] = b"SILENT-ACCOUNT-BACKUP\n";
/// Binds the sealed payload to its use.
pub const AAD: &[u8] = b"silent-account-backup-v1";
/// Bump when the payload changes in a way older Silent versions cannot read.
pub const VERSION: u32 = 1;

const SP_PLUGIN_ID: &str = "sp";

/// The receive and change indexes one descriptor handed out.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Tip {
    pub descriptor: String,
    pub receive: u32,
    pub change: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Label {
    pub kind: String,
    pub id: String,
    pub text: String,
}

/// What a backup seals.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Payload {
    pub version: u32,
    pub config: Config,
    /// The block SP scanning starts from, None for an account with no SP scan.
    pub scan_start: Option<u32>,
    pub tips: Vec<Tip>,
    pub labels: Vec<Label>,
}

/// Why a backup was not made, read or restored.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Failure {
    /// Not a backup, or a damaged one.
    Malformed(String),
    WrongPassword,
    /// Made by a newer Silent, or for another kind of account.
    Unsupported(String),
    /// An account of that name exists: a restore never replaces one.
    Exists(String),
    Other(String),
}

impl Failure {
    fn kind(&self) -> BackupFailure {
        match self {
            Self::Malformed(_) => BackupFailure::Malformed,
            Self::WrongPassword => BackupFailure::WrongPassword,
            Self::Unsupported(_) => BackupFailure::Unsupported,
            Self::Exists(_) => BackupFailure::Exists,
            Self::Other(_) => BackupFailure::Other,
        }
    }

    fn message(&self) -> String {
        match self {
            Self::Malformed(error) => format!("Not a valid account backup: {error}"),
            Self::WrongPassword => "Wrong password".into(),
            Self::Unsupported(error) | Self::Exists(error) | Self::Other(error) => error.clone(),
        }
    }
}

/// The label kind of its backup name.
fn label_kind(name: &str) -> Option<LabelKind> {
    match name {
        "address" => Some(LabelKind::Address),
        "outpoint" => Some(LabelKind::Outpoint),
        "transaction" => Some(LabelKind::Transaction),
        _ => None,
    }
}

fn label_name(kind: LabelKind) -> &'static str {
    match kind {
        LabelKind::Address => "address",
        LabelKind::Outpoint => "outpoint",
        _ => "transaction",
    }
}

pub(crate) fn labels_of(entries: Vec<LabelEntry>) -> Vec<Label> {
    entries
        .into_iter()
        .map(|entry| Label {
            kind: label_name(entry.kind).into(),
            id: entry.id,
            text: entry.text,
        })
        .collect()
}

/// The indexes `scanner` handed out.
pub(crate) fn scanner_tip(scanner: &ElectrumScanner) -> Tip {
    let (receive, change) = scanner.generated_addresses();
    Tip {
        descriptor: scanner.descriptor().to_string(),
        receive: receive.last().map_or(0, |entry| entry.index),
        change: change.last().map_or(0, |entry| entry.index),
    }
}

/// Moves `scanner` to the indexes of the tip in `tips` for its descriptor, and saves them.
pub(crate) fn restore_scanner_tip(scanner: &ElectrumScanner, tips: &[Tip]) -> Result<(), String> {
    let descriptor = scanner.descriptor().to_string();
    let Some(tip) = tips.iter().find(|tip| tip.descriptor == descriptor) else {
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

/// The backup of the account `config` describes, with the state the account holds.
pub(crate) fn seal(
    config: &Config,
    scan_start: Option<u32>,
    tips: Vec<Tip>,
    labels: Vec<Label>,
    password: String,
) -> Result<Vec<u8>, String> {
    if password.is_empty() {
        return Err("The password is empty".into());
    }
    let payload = Payload {
        version: VERSION,
        config: config.clone(),
        scan_start,
        tips,
        labels,
    };
    let json = Zeroizing::new(serde_json::to_vec(&payload).map_err(|e| e.to_string())?);
    let envelope = seal_with_aad(&json, AAD, Password::new(password)).map_err(|e| e.to_string())?;
    let mut bytes = MAGIC.to_vec();
    bytes.extend(serde_json::to_vec(&envelope).map_err(|e| e.to_string())?);
    Ok(bytes)
}

/// The payload `bytes` seals, opened with `password`.
pub fn open(bytes: &[u8], password: String) -> Result<Payload, Failure> {
    let Some(sealed) = bytes.strip_prefix(MAGIC) else {
        return Err(Failure::Malformed("no account backup header".into()));
    };
    let envelope: SecretEnvelope =
        serde_json::from_slice(sealed).map_err(|e| Failure::Malformed(e.to_string()))?;
    let json = open_with_aad(&envelope, AAD, Password::new(password)).map_err(|e| match e {
        KeyError::WrongPassword => Failure::WrongPassword,
        KeyError::UnsupportedEnvelopeVersion { .. }
        | KeyError::UnsupportedKdf(_)
        | KeyError::UnsupportedCipher(_) => Failure::Unsupported(e.to_string()),
        other => Failure::Malformed(other.to_string()),
    })?;
    let version: serde_json::Value =
        serde_json::from_slice(&json).map_err(|e| Failure::Malformed(e.to_string()))?;
    match version.get("version").and_then(serde_json::Value::as_u64) {
        Some(found) if found > u64::from(VERSION) => {
            return Err(Failure::Unsupported(format!(
                "This backup was made by a newer Silent (version {found})"
            )))
        }
        Some(_) => {}
        None => return Err(Failure::Malformed("no payload version".into())),
    }
    let mut payload: Payload =
        serde_json::from_value(version).map_err(|e| Failure::Malformed(e.to_string()))?;
    payload.config.data_dir = datadir();
    Ok(payload)
}

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

/// The receive kind of a descriptor's addresses, by its script.
fn tip_kind(descriptor: &str) -> String {
    match Descriptor::<DescriptorPublicKey>::from_str(descriptor) {
        Ok(Descriptor::Wpkh(_)) => SEGWIT_KIND.into(),
        _ => TAPROOT_KIND.into(),
    }
}

fn contents(payload: &Payload) -> BackupContents {
    let config = &payload.config;
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
        network: config.network,
        endpoints,
        tips: payload
            .tips
            .iter()
            .map(|tip| BackupTip {
                kind: tip_kind(&tip.descriptor),
                receive: tip.receive,
                change: tip.change,
            })
            .collect(),
        scan_start: payload.scan_start.unwrap_or(0),
        labels: payload.labels.len() as u32,
        signers: signers(config),
        saved_psbts_excluded: config.plugin_id == MUSIG_PLUGIN_ID,
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
        endpoints: vec![],
        tips: vec![],
        scan_start: 0,
        labels: 0,
        signers: vec![],
        saved_psbts_excluded: false,
    }
}

/// What the backup `bytes` holds, opened with `password`, and what restoring it needs.
pub fn inspect_account_backup(bytes: &[u8], password: String) -> BackupContents {
    match open(bytes, password) {
        Ok(payload) => contents(&payload),
        Err(failure) => contents_failed(failure),
    }
}

pub(crate) fn backup_result(result: Result<Vec<u8>, String>) -> AccountBackup {
    match result {
        Ok(bytes) => AccountBackup {
            is_ok: true,
            error: String::new(),
            bytes,
        },
        Err(error) => AccountBackup {
            is_ok: false,
            error,
            bytes: vec![],
        },
    }
}

/// Opens the restored account offline, puts the backup's indexes and labels in it,
/// then closes it, flushing its stores.
fn apply_state(payload: &Payload) -> Result<(), String> {
    if payload.config.plugin_id == MUSIG_PLUGIN_ID {
        let mut account = crate::musig::new_musig_account_offline(&payload.config);
        if !account.is_ok() {
            return Err(account.get_error());
        }
        let applied = account.restore_state(&payload.tips, &payload.labels);
        let flushed = account.shutdown_backend();
        applied?;
        return if flushed.is_empty() {
            Ok(())
        } else {
            Err(flushed)
        };
    }
    let mut account = crate::account::new_account_offline(&payload.config);
    if !account.is_ok() {
        return Err(account.get_error());
    }
    let applied = account.restore_state(&payload.tips, &payload.labels);
    let flushed = account.shutdown_backend();
    applied?;
    if flushed.is_empty() {
        Ok(())
    } else {
        Err(flushed)
    }
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

fn restore(plugin_id: &str, bytes: &[u8], password: String) -> Result<String, Failure> {
    let mut payload = open(bytes, password)?;
    // The restored account scans from where the backed up one did: its fresh store is
    // seeded with the backup's scan start.
    if payload.config.plugin_id == SP_PLUGIN_ID {
        payload.config.scan_start = payload.scan_start;
    }
    let config = &payload.config;
    if config.plugin_id != plugin_id {
        return Err(Failure::Unsupported(format!(
            "This backup is of a '{}' account, not a '{plugin_id}' one",
            config.plugin_id
        )));
    }
    if ![SP_PLUGIN_ID, MUSIG_PLUGIN_ID].contains(&plugin_id) {
        return Err(Failure::Unsupported(format!(
            "No '{plugin_id}' accounts here"
        )));
    }
    if plugin_id == SP_PLUGIN_ID {
        config.to_sp_config().map_err(Failure::Malformed)?;
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
    if let Err(error) = apply_state(&payload) {
        // Nothing partial stays: the account created goes.
        if let Err(removal) = crate::account_storage::delete_owned(plugin_id, &config.account_name)
        {
            log::error!("restore: removing the partial account failed: {removal}");
        }
        return Err(Failure::Other(error));
    }
    Ok(config.account_name.clone())
}

/// Restores the `plugin_id` account the backup `bytes` holds, opened with `password`, as
/// a new account of the same name: refused when one exists.
pub fn restore_account_backup(plugin_id: &str, bytes: &[u8], password: String) -> BackupRestore {
    match restore(plugin_id, bytes, password) {
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
