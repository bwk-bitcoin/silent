//! BIP139 wallet backup documents: the fields Silent writes and reads.
//!
//! Every field but `accounts` is optional, and a reader ignores the fields it does not
//! know (BIP139, Backwards Compatibility): only what Silent uses is modelled here.

use serde::{Deserialize, Serialize};

/// The number of this BIP, in a document's `bip` field.
pub const BIP: u32 = 139;
/// The version of the format BIP139 defines.
pub const VERSION: u32 = 1;

/// The `type` of an account holding a BIP380 output descriptor.
pub const TYPE_BIP380: &str = "bip_380";
/// The `type` of an account holding a BIP392 silent payment descriptor.
pub const TYPE_BIP392: &str = "bip_392";

/// A BIP139 wallet backup: a wallet and its accounts.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct WalletBackup {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub version: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bip: Option<u32>,
    #[serde(default, alias = "alias", skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    pub accounts: Vec<Account>,
    /// `bitcoin`, `testnet3`, `testnet4`, `signet` or `regtest`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub network: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub proprietary: Option<serde_json::Value>,
}

/// One account of a wallet backup.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Account {
    #[serde(default, alias = "alias", skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    #[serde(default, rename = "type", skip_serializing_if = "Option::is_none")]
    pub kind: Option<String>,
    /// A string or an object, as `kind` defines it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub descriptor: Option<serde_json::Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub receive_index: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub change_index: Option<u32>,
    /// The account's creation time, in unix seconds.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub timestamp: Option<u64>,
    /// The account's creation time, in block height.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub block_height: Option<u32>,
    #[serde(default, alias = "labels", skip_serializing_if = "Option::is_none")]
    pub bip329_labels: Option<Vec<Label>>,
}

/// A BIP329 label.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Label {
    /// `tx`, `addr`, `pubkey`, `input`, `output` or `xpub`.
    #[serde(rename = "type")]
    pub kind: String,
    #[serde(rename = "ref")]
    pub reference: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub label: Option<String>,
}

impl WalletBackup {
    /// The document `bytes` holds, refused without an account as BIP139 requires.
    pub fn parse(bytes: &[u8]) -> Result<Self, String> {
        let backup: Self = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
        if backup.accounts.is_empty() {
            return Err("a BIP139 wallet backup holds at least one account".into());
        }
        Ok(backup)
    }
}

impl Account {
    /// The descriptor as text: Silent's account types all hold a string.
    pub fn descriptor_text(&self) -> Result<Option<&str>, String> {
        match &self.descriptor {
            None => Ok(None),
            Some(serde_json::Value::String(text)) => Ok(Some(text)),
            Some(_) => Err("an account descriptor that is not a string".into()),
        }
    }
}
