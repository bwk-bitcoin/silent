//! BIP138 backup of a Multikey account's descriptor: the descriptor encrypted to its
//! keys, so that any one of their xpubs decrypts it.
//!
//! bip138 reads miniscript descriptors, which have no musig(): the descriptor is
//! handed over as its BIP390 text, with the encryption keys BIP138 picks from it.
//! Outside musig() a key encrypts when it is an xpub with a trailing derivation or a
//! wildcard; inside, each participant xpub encrypts by its root. A literal key, a bare
//! xpub outside musig() and the BIP341 NUMS point never do, and a root another
//! expression puts on chain is dropped.

use std::collections::BTreeSet;
use std::path::Path;

use bip138::{
    descriptor::{dpk_to_pk, dpk_to_root_pk, BIP341_NUMS},
    Content, EncryptedBackup,
};
use bitcoin::bip32::DerivationPath;
use bitcoin::secp256k1::PublicKey;
use bwk::bwk_descriptor::musig::parse_musig_descriptor;
use bwk::bwk_keys::musig::MusigKey;
use bwk::bwk_sign::miniscript::{
    descriptor::{SinglePubKey, Wildcard},
    DescriptorPublicKey, ForEachKey,
};

/// A backup written to disk, and the key expressions left out of its encryption keys.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WrittenBackup {
    /// Each excluded expression, as the descriptor writes it: its holder cannot
    /// decrypt the backup with it.
    pub excluded: Vec<String>,
}

/// The encryption keys BIP138 picks from a descriptor, their origin paths, and the
/// expressions it leaves out.
#[derive(Debug, Default)]
struct Recipients {
    keys: Vec<(PublicKey, Option<DerivationPath>)>,
    excluded: Vec<String>,
    exposed: BTreeSet<[u8; 32]>,
}

fn xonly(key: &PublicKey) -> [u8; 32] {
    key.x_only_public_key().0.serialize()
}

fn origin_path(key: &DescriptorPublicKey) -> Option<DerivationPath> {
    match key {
        DescriptorPublicKey::Single(k) => k.origin.as_ref().map(|(_, path)| path.clone()),
        DescriptorPublicKey::XPub(k) => k.origin.as_ref().map(|(_, path)| path.clone()),
        DescriptorPublicKey::MultiXPub(k) => k.origin.as_ref().map(|(_, path)| path.clone()),
    }
}

/// The x-only key `key` puts on chain as is: a literal key, or the root of a bare
/// xpub (no derivation, no wildcard, no multipath).
fn exposed_root(key: &DescriptorPublicKey) -> Option<[u8; 32]> {
    match key {
        DescriptorPublicKey::Single(k) => Some(match k.key {
            SinglePubKey::FullKey(pk) => pk.inner.x_only_public_key().0.serialize(),
            SinglePubKey::XOnly(xonly) => xonly.serialize(),
        }),
        DescriptorPublicKey::XPub(k)
            if k.derivation_path.is_empty() && k.wildcard == Wildcard::None =>
        {
            Some(xonly(&k.xkey.public_key))
        }
        DescriptorPublicKey::XPub(_) | DescriptorPublicKey::MultiXPub(_) => None,
    }
}

impl Recipients {
    fn of(descriptor: &str) -> Result<Self, String> {
        let parsed = parse_musig_descriptor(descriptor).map_err(|e| e.to_string())?;
        let mut recipients = Self::default();
        parsed.for_each_key(|key| {
            match key {
                MusigKey::Plain(k) => recipients.add_plain(k),
                MusigKey::Musig(aggregate) => {
                    for cosigner in aggregate.cosigners() {
                        recipients.add_participant(cosigner);
                    }
                }
            }
            true
        });
        recipients.drop_exposed();
        Ok(recipients)
    }

    /// A key outside musig(): it encrypts only with a trailing derivation or a
    /// wildcard. The NUMS point is no one's key, so it is left out silently.
    fn add_plain(&mut self, key: &DescriptorPublicKey) {
        let exposed = exposed_root(key);
        self.exposed.extend(exposed);
        match dpk_to_pk(key) {
            Ok(root) if xonly(&root) == BIP341_NUMS => {}
            Ok(root) => self.keys.push((root, origin_path(key))),
            Err(_) if exposed == Some(BIP341_NUMS) => {}
            Err(_) => self.excluded.push(key.to_string()),
        }
    }

    /// A musig() participant: an xpub encrypts by its root, with or without
    /// derivation; a literal key never does.
    fn add_participant(&mut self, key: &DescriptorPublicKey) {
        match dpk_to_root_pk(key) {
            Ok(root) if xonly(&root) == BIP341_NUMS => {}
            Ok(root) => self.keys.push((root, origin_path(key))),
            Err(_) => self.excluded.push(key.to_string()),
        }
    }

    /// A root a literal key or a bare xpub puts on chain would let anyone who saw it
    /// spent decrypt the backup: it is left out.
    fn drop_exposed(&mut self) {
        let exposed = &self.exposed;
        let excluded = &mut self.excluded;
        self.keys.retain(|(key, _)| {
            if !exposed.contains(&xonly(key)) {
                return true;
            }
            let text = key.to_string();
            if !excluded.contains(&text) {
                excluded.push(text);
            }
            false
        });
    }

    fn keys(&self) -> Vec<PublicKey> {
        let keys: BTreeSet<_> = self.keys.iter().map(|(key, _)| *key).collect();
        keys.into_iter().collect()
    }

    fn derivation_paths(&self) -> Vec<DerivationPath> {
        let paths: BTreeSet<_> = self
            .keys
            .iter()
            .filter_map(|(_, path)| path.clone())
            .collect();
        paths.into_iter().collect()
    }
}

/// The BIP138 backup of `descriptor`, and the expressions left out of its keys.
/// `store_common_paths` keeps the common derivation paths (BIP48, BIP87) in the
/// clear part, which tells a restore where to look; leaving them out is the privacy
/// mode. Refused when no key is left to encrypt to.
pub fn encrypt_descriptor(
    descriptor: &str,
    store_common_paths: bool,
) -> Result<(Vec<u8>, Vec<String>), String> {
    let recipients = Recipients::of(descriptor)?;
    let encrypted = EncryptedBackup::new()
        .set_payload(&descriptor.trim().to_string())
        .map_err(|e| format!("{e:?}"))?
        .set_content_type(Content::Bip380)
        .set_keys(recipients.keys())
        .set_derivation_paths(recipients.derivation_paths())
        .set_store_common_paths(store_common_paths)
        .encrypt()
        .map_err(|e| format!("{e:?}"))?;
    Ok((encrypted.bytes, recipients.excluded))
}

/// Writes the BIP138 backup of `descriptor` to `path`, replacing any file there.
pub fn write_descriptor_backup(
    descriptor: &str,
    path: &Path,
    store_common_paths: bool,
) -> Result<WrittenBackup, String> {
    let (bytes, excluded) = encrypt_descriptor(descriptor, store_common_paths)?;
    std::fs::write(path, bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(WrittenBackup { excluded })
}
