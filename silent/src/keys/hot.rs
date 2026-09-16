//! Hot key material: a fresh mnemonic drawn from `getrandom`, and the fingerprint a
//! mnemonic derives to on a given network. `HotSigner::new` is never used here: it
//! asserts on mainnet, which is exactly where a hot key has to work.

use std::str::FromStr;

use bitcoin::{
    bip32::{ChildNumber, DerivationPath, Fingerprint},
    Network, NetworkKind,
};
use bwk_sp::{
    bwk_sign::{
        bip39,
        bwk_descriptor::{
            descriptor::Descriptor,
            sp_descriptor::SpDescriptor,
            sp_key::{SpKey, SpScanKey},
        },
        error::Error as HotSignerError,
        hot_signer::HotSigner,
    },
    bwk_tx::bwk_psbt::{PsbtV2, TxModifiable},
    receiver::{bip352_base_derivation, scan_path, spend_path},
    signer::SpSigner,
};
use zeroize::Zeroizing;

use crate::keys::{
    crypto::{self, Password},
    error::Error,
    psbt::parse_v2,
    record::{KeyRecord, SecretEnvelope},
};

pub const MNEMONIC_WORDS_SHORT: usize = 12;
pub const MNEMONIC_WORDS_LONG: usize = 24;
const ENTROPY_BYTES_SHORT: usize = 16;
const ENTROPY_BYTES_LONG: usize = 32;

/// Account index the silent-payment spend key is derived at.
const SP_ACCOUNT_INDEX: u32 = 0;

/// Draws entropy from `getrandom` and builds a mnemonic from it, so behavior is
/// identical on every network, mainnet included.
pub fn generate_mnemonic(word_count: usize) -> Result<Zeroizing<String>, Error> {
    let entropy_len = match word_count {
        MNEMONIC_WORDS_SHORT => ENTROPY_BYTES_SHORT,
        MNEMONIC_WORDS_LONG => ENTROPY_BYTES_LONG,
        other => return Err(Error::InvalidWordCount(other)),
    };

    let mut entropy = Zeroizing::new(vec![0u8; entropy_len]);
    getrandom::getrandom(entropy.as_mut()).map_err(|e| Error::Random(e.to_string()))?;
    let mnemonic = bip39::Mnemonic::from_entropy(entropy.as_ref())
        .map_err(|e| Error::Mnemonic(e.to_string()))?;
    Ok(Zeroizing::new(mnemonic.to_string()))
}

/// Derives on the manager's network, so the same seed on two networks is two separate
/// records, each with its own alias, routes and cache.
pub fn fingerprint_of(mnemonic: &str, network: Network) -> Result<Fingerprint, Error> {
    bip39::Mnemonic::parse(mnemonic).map_err(|e| Error::Mnemonic(e.to_string()))?;
    let signer = HotSigner::new_from_mnemonics(network, mnemonic)
        .map_err(|e| Error::Signer(e.to_string()))?;
    Ok(signer.fingerprint())
}

/// Derives the fingerprint and seals `mnemonic` under `password` for `network`.
/// Blocks on Argon2id: call from a QThread, never from the GUI thread.
pub fn seal_mnemonic(
    mnemonic: &str,
    network: Network,
    password: Password,
) -> Result<(Fingerprint, SecretEnvelope), Error> {
    let fingerprint = fingerprint_of(mnemonic, network)?;
    let envelope = crypto::seal(mnemonic.as_bytes(), fingerprint, network, password)?;
    Ok((fingerprint, envelope))
}

fn tap_key_sig_count(psbt: &PsbtV2) -> usize {
    psbt.inputs
        .iter()
        .filter(|input| input.psbt.tap_key_sig.is_some())
        .count()
}

/// The hardened account index the silent-payment spend key is derived at.
pub(crate) fn sp_account() -> Result<ChildNumber, Error> {
    ChildNumber::from_hardened_idx(SP_ACCOUNT_INDEX).map_err(|e| Error::SpSign(e.to_string()))
}

/// Builds this hot key's BIP392 sp() descriptor, origin included, for account
/// creation. The descriptor is watch-only: it carries the private scan key and the
/// public spend key only, never the spend secret. Signing re-opens the sealed
/// envelope (`sign_hot`) instead of relying on a spend secret in the descriptor.
/// Blocks on Argon2id while opening the envelope, so it must not run on a GUI
/// thread.
pub fn sp_descriptor(
    record: &KeyRecord,
    network: Network,
    password: Password,
) -> Result<SpDescriptor, Error> {
    let mnemonic = crypto::open_mnemonic(record, network, password)?;
    let signer = HotSigner::new_from_mnemonics(network, mnemonic.as_str())
        .map_err(|e| Error::Signer(e.to_string()))?;
    let account = sp_account()?;

    // FIXME: bwk's Config::new builds this descriptor with origin: None, so it is
    // rebuilt here. Upstream ask: a watch-only sp() builder that sets the origin.
    let origin = (
        signer.fingerprint(),
        DerivationPath::from(bip352_base_derivation(network, account)),
    );
    Ok(SpDescriptor::Packed {
        origin: Some(origin),
        key: SpKey::Scan(SpScanKey {
            scan_key: signer.private_key_at(&scan_path(network, account)),
            spend_key: signer.public_key_at(&spend_path(network, account)),
            network: NetworkKind::from(network),
        }),
    })
}

/// Signs `psbt_v2_bytes` with this key's hot secret and returns the re-serialized
/// PSBTv2. Blocks on Argon2id while opening the envelope, so it must not run on a
/// GUI thread.
pub fn sign_hot(
    record: &KeyRecord,
    network: Network,
    password: Password,
    psbt_v2_bytes: &[u8],
) -> Result<Vec<u8>, Error> {
    let mnemonic = crypto::open_mnemonic(record, network, password)?;

    let mut psbt = parse_v2(psbt_v2_bytes)?;

    let descriptors = record
        .descriptors
        .iter()
        .map(|entry| {
            Descriptor::from_str(&entry.descriptor)
                .map_err(|e| Error::DescriptorParse(e.to_string()))
        })
        .collect::<Result<Vec<_>, _>>()?;

    // Only a record holding the sp() descriptor may run `SpSigner::sign`: for any
    // other key it errors out from inside `complete_output_scripts` on a PSBT the
    // sp() key hasn't derived outputs on yet, hiding the intended SpOutputsPending.
    let has_sp_descriptor = descriptors.iter().any(Descriptor::is_sp);
    let sp_signed_any = if has_sp_descriptor {
        let signed_before = tap_key_sig_count(&psbt);
        let sp_signer = SpSigner::from_mnemonic(mnemonic.as_str(), network, sp_account()?)
            .map_err(|e| Error::SpSign(e.to_string()))?;
        sp_signer
            .sign(&mut psbt)
            .map_err(|e| Error::SpSign(e.to_string()))?;
        tap_key_sig_count(&psbt) > signed_before
    } else {
        false
    };

    // `into_bitcoin_psbt` fails on a missing output script: reaching that here means
    // the sp() key has not derived the outputs yet, so this key must not sign now.
    let mut v0 = psbt
        .clone()
        .into_bitcoin_psbt()
        .map_err(|_| Error::SpOutputsPending)?;

    let signer = HotSigner::new_from_mnemonics(network, mnemonic.as_str())
        .map_err(|e| Error::Signer(e.to_string()))?;
    let mut miniscript_signed_any = false;
    for descriptor in &descriptors {
        if descriptor.is_sp() {
            continue;
        }
        let Some(miniscript) = descriptor.as_miniscript() else {
            continue;
        };
        match signer.inner_sign(&mut v0, miniscript) {
            Ok(()) => miniscript_signed_any = true,
            // A descriptor that matches no input in this PSBT is normal in
            // multi-account signing, not a failure.
            Err(HotSignerError::SigningInfo) => {
                log::debug!("hot sign: descriptor matched no input");
            }
            Err(e) => return Err(Error::Signer(e.to_string())),
        }
    }

    // FIXME: bwk keeps `set_v0_maps` and an SP-aware `validated_v0` private, and the
    // public `validated_v0` refuses any PSBT carrying SP output info, so both have to
    // be redone here. Upstream ask: make them public.
    for (input, v0_input) in psbt.inputs.iter_mut().zip(v0.inputs) {
        input.psbt = v0_input;
    }
    for (output, v0_output) in psbt.outputs.iter_mut().zip(v0.outputs) {
        output.psbt = v0_output;
    }

    if !sp_signed_any && !miniscript_signed_any {
        return Err(Error::NothingSigned);
    }

    psbt.tx_modifiable = Some(TxModifiable::none());

    psbt.serialize()
        .map_err(|e| Error::PsbtSerialize(e.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    const TEST_MNEMONIC: &str = "abandon abandon abandon abandon abandon abandon \
        abandon abandon abandon abandon abandon about";
    const TEST_FINGERPRINT_BYTES: [u8; 4] = [0x73, 0xc5, 0xda, 0x0a];

    #[test]
    fn generate_produces_twelve_words() {
        let mnemonic = generate_mnemonic(MNEMONIC_WORDS_SHORT).expect("generate succeeds");
        let word_count = mnemonic.split_whitespace().count();
        assert_eq!(
            word_count, MNEMONIC_WORDS_SHORT,
            "expected {MNEMONIC_WORDS_SHORT} words, got {word_count}"
        );
        bip39::Mnemonic::parse(mnemonic.as_str()).expect("mnemonic reparses");
    }

    #[test]
    fn generate_produces_twenty_four_words() {
        let mnemonic = generate_mnemonic(MNEMONIC_WORDS_LONG).expect("generate succeeds");
        let word_count = mnemonic.split_whitespace().count();
        assert_eq!(
            word_count, MNEMONIC_WORDS_LONG,
            "expected {MNEMONIC_WORDS_LONG} words, got {word_count}"
        );
        bip39::Mnemonic::parse(mnemonic.as_str()).expect("mnemonic reparses");
    }

    #[test]
    fn generate_works_on_mainnet() {
        let mnemonic = generate_mnemonic(MNEMONIC_WORDS_SHORT).expect("generate succeeds");
        let fingerprint = fingerprint_of(mnemonic.as_str(), Network::Bitcoin)
            .expect("fingerprint derives on mainnet without aborting");
        assert_ne!(
            fingerprint.as_bytes(),
            &[0u8; 4],
            "expected a real fingerprint, got the zero value"
        );
    }

    #[test]
    fn generate_rejects_an_unsupported_word_count() {
        let result = generate_mnemonic(18);
        assert!(
            matches!(result, Err(Error::InvalidWordCount(18))),
            "expected InvalidWordCount(18), got {result:?}"
        );
    }

    #[test]
    fn two_generations_differ() {
        let first = generate_mnemonic(MNEMONIC_WORDS_SHORT).expect("first generate");
        let second = generate_mnemonic(MNEMONIC_WORDS_SHORT).expect("second generate");
        assert_ne!(
            first.as_str(),
            second.as_str(),
            "expected two generations to differ"
        );
    }

    #[test]
    fn fingerprint_of_the_test_vector() {
        let fingerprint =
            fingerprint_of(TEST_MNEMONIC, Network::Regtest).expect("fingerprint derives");
        assert_eq!(
            fingerprint,
            Fingerprint::from(TEST_FINGERPRINT_BYTES),
            "expected the standard test vector's fingerprint, got {fingerprint}"
        );
    }

    fn test_record() -> KeyRecord {
        let fingerprint = Fingerprint::from(TEST_FINGERPRINT_BYTES);
        let mut record = KeyRecord::new(fingerprint, Network::Regtest, "alias".to_string());
        record.secret = Some(
            crypto::seal(
                TEST_MNEMONIC.as_bytes(),
                fingerprint,
                Network::Regtest,
                Password::new("password".to_string()),
            )
            .expect("seal succeeds"),
        );
        record
    }

    #[test]
    fn sp_descriptor_is_watch_only_and_keeps_the_origin_fingerprint() {
        use bitcoin::secp256k1::Secp256k1;

        let record = test_record();
        let descriptor = sp_descriptor(
            &record,
            Network::Regtest,
            Password::new("password".to_string()),
        )
        .expect("sp_descriptor succeeds");

        let secp = Secp256k1::new();
        assert!(
            descriptor
                .is_watch_only(&secp)
                .expect("is_watch_only succeeds"),
            "expected a watch-only descriptor, got {descriptor:?}"
        );
        assert_eq!(
            descriptor.fingerprint(),
            Some(Fingerprint::from(TEST_FINGERPRINT_BYTES)),
            "expected the origin fingerprint to match the signer's"
        );

        let signer =
            HotSigner::new_from_mnemonics(Network::Regtest, TEST_MNEMONIC).expect("signer builds");
        let account = sp_account().expect("sp account index");
        let expected_spend_key = signer.public_key_at(&spend_path(Network::Regtest, account));
        assert_eq!(
            descriptor
                .spend_public_key(&secp)
                .expect("spend_public_key succeeds"),
            expected_spend_key,
            "expected the descriptor's spend key to match the derived one"
        );
    }
}
