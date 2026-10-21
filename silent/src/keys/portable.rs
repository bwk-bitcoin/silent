use std::{fs, path::Path, str::FromStr};

use bitcoin::{
    bip32::{DerivationPath, Fingerprint, Xpub},
    Network, NetworkKind,
};
use serde::Deserialize;

use crate::keys::{
    error::Error,
    record::{validate_xpub_chain, KeyRecord, RouteKind, Routes, XpubEntry},
};

const VERSION: u32 = 1;

#[derive(Debug, thiserror::Error)]
pub enum PortableError {
    #[error("cannot read key file: {0}")]
    Read(#[from] std::io::Error),
    #[error("invalid key file: {0}")]
    Format(String),
    #[error("key file version {0} is newer than supported version 1")]
    Version(u32),
    #[error("key file network {found} does not match {expected}")]
    Network { expected: Network, found: Network },
    #[error(transparent)]
    Key(#[from] Error),
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct PortableKey {
    version: u32,
    network: Network,
    fingerprint: String,
    #[serde(default)]
    alias: String,
    #[serde(default)]
    routes: Vec<String>,
    #[serde(default)]
    xpubs: Vec<PortableXpub>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct PortableXpub {
    path: String,
    xpub: String,
}

pub fn read(path: &Path, network: Network) -> Result<KeyRecord, PortableError> {
    parse(&fs::read_to_string(path)?, network)
}

pub fn parse(text: &str, network: Network) -> Result<KeyRecord, PortableError> {
    let file: PortableKey =
        serde_json::from_str(text).map_err(|e| PortableError::Format(e.to_string()))?;
    if file.version > VERSION {
        return Err(PortableError::Version(file.version));
    }
    if file.version != VERSION {
        return Err(PortableError::Format("unsupported key file version".into()));
    }
    if !matches!(
        file.network,
        Network::Bitcoin | Network::Testnet | Network::Signet | Network::Regtest
    ) {
        return Err(PortableError::Format("unsupported key file network".into()));
    }
    if file.network != network {
        return Err(PortableError::Network {
            expected: network,
            found: file.network,
        });
    }
    let fingerprint = Fingerprint::from_str(file.fingerprint.trim())
        .map_err(|e| PortableError::Format(e.to_string()))?;
    let mut record = KeyRecord::new(fingerprint, network, file.alias);
    record.routes = routes(file.routes)?;
    record.xpubs = file
        .xpubs
        .into_iter()
        .map(|entry| xpub(entry, fingerprint, network))
        .collect::<Result<Vec<_>, _>>()?;
    validate(&record)?;
    Ok(record)
}

fn routes(names: Vec<String>) -> Result<Routes, PortableError> {
    let mut routes = Vec::new();
    for name in names {
        routes.push(match name.as_str() {
            "usb" => RouteKind::Usb,
            "qr" => RouteKind::Qr,
            "sdcard" | "sd_card" => RouteKind::SdCard,
            "hot" => {
                return Err(PortableError::Format(
                    "a public key file cannot supply a hot secret".into(),
                ))
            }
            _ => return Err(PortableError::Format(format!("unknown key route '{name}'"))),
        });
    }
    if routes.is_empty() {
        routes.push(RouteKind::SdCard);
    }
    Routes::try_from(routes).map_err(PortableError::from)
}

fn xpub(
    entry: PortableXpub,
    fingerprint: Fingerprint,
    network: Network,
) -> Result<XpubEntry, PortableError> {
    let path =
        DerivationPath::from_str(&entry.path).map_err(|e| PortableError::Format(e.to_string()))?;
    let xpub = Xpub::from_str(&entry.xpub).map_err(|e| PortableError::Format(e.to_string()))?;
    if xpub.network != NetworkKind::from(network)
        || usize::from(xpub.depth) != path.len()
        || path
            .into_iter()
            .last()
            .is_some_and(|child| *child != xpub.child_number)
        || (path.is_empty() && xpub.fingerprint() != fingerprint)
        || (path.len() == 1 && xpub.parent_fingerprint != fingerprint)
    {
        return Err(PortableError::Format(format!(
            "xpub does not match its network, origin or path {}",
            entry.path
        )));
    }
    Ok(XpubEntry {
        path,
        xpub,
        used: false,
    })
}

pub fn validate(record: &KeyRecord) -> Result<(), PortableError> {
    record.check_version()?;
    for (index, entry) in record.xpubs.iter().enumerate() {
        if record.xpubs[..index]
            .iter()
            .any(|known| known.path == entry.path)
        {
            return Err(PortableError::Format(format!(
                "duplicate xpub path {}",
                entry.path
            )));
        }
    }
    validate_xpub_chain(&record.xpubs)?;
    Ok(())
}
