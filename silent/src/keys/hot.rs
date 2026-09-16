//! Hot key material: a fresh mnemonic drawn from `getrandom`, and the fingerprint a
//! mnemonic derives to on a given network. `HotSigner::new` is never used here: it
//! asserts on mainnet, which is exactly where a hot key has to work.

use bitcoin::{bip32::Fingerprint, Network};
use bwk_sp::bwk_sign::{bip39, hot_signer::HotSigner};
use zeroize::Zeroizing;

use crate::keys::{
    crypto::{self, Password},
    error::Error,
    record::SecretEnvelope,
};

pub const MNEMONIC_WORDS_SHORT: usize = 12;
pub const MNEMONIC_WORDS_LONG: usize = 24;
const ENTROPY_BYTES_SHORT: usize = 16;
const ENTROPY_BYTES_LONG: usize = 32;

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
}
