//! Password based sealing and opening of a key's secret. Argon2id derives the key,
//! XChaCha20-Poly1305 encrypts, and the associated data binds the ciphertext to one
//! fingerprint on one network so a blob moved to another record or another network
//! fails to open.

use argon2::{Algorithm, Argon2, Params, Version};
use base64::prelude::BASE64_STANDARD;
use base64::Engine;
use bitcoin::{bip32::Fingerprint, Network};
use chacha20poly1305::aead::{Aead, Payload};
use chacha20poly1305::{Key, KeyInit, XChaCha20Poly1305, XNonce};
use zeroize::Zeroizing;

use crate::keys::{
    error::Error,
    network_name,
    record::{KeyRecord, SecretEnvelope},
};

/// Bump when the sealed envelope's format changes in a way older Silent versions
/// cannot open.
pub const SECRET_ENVELOPE_VERSION: u32 = 1;

const KDF_NAME: &str = "argon2id";
const CIPHER_NAME: &str = "xchacha20poly1305";

/// OWASP-recommended Argon2id cost for interactive use, 64 MiB of memory.
const M_COST_KIB: u32 = 65_536;
const T_COST: u32 = 3;
const P_COST: u32 = 1;

/// An envelope must never open with parameters weaker than what this build would
/// produce, so the minimums are the compiled costs themselves.
const MIN_M_COST_KIB: u32 = M_COST_KIB;
const MIN_T_COST: u32 = T_COST;
const MIN_P_COST: u32 = P_COST;

const SALT_LEN: usize = 16;
/// XChaCha20 nonce length.
const NONCE_LEN: usize = 24;
const KEY_LEN: usize = 32;

const AAD_PREFIX: &[u8] = b"silent-key-v1";

/// A password held only long enough to seal or open one envelope. Not `Clone`: a
/// caller that needs two operations asks the user twice.
pub struct Password(Zeroizing<String>);

impl Password {
    pub fn new(value: String) -> Self {
        Self(Zeroizing::new(value))
    }
}

impl std::fmt::Debug for Password {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Password(hidden)")
    }
}

/// Recomputed on every seal and open, never stored in the envelope: this is what
/// makes a ciphertext moved to another record or another network fail to open
/// instead of decrypting into the wrong place.
fn associated_data(fingerprint: Fingerprint, network: Network) -> Result<Vec<u8>, Error> {
    let mut aad = Vec::with_capacity(AAD_PREFIX.len() + 4 + 8);
    aad.extend_from_slice(AAD_PREFIX);
    aad.extend_from_slice(fingerprint.as_bytes());
    aad.extend_from_slice(network_name(network)?.as_bytes());
    Ok(aad)
}

fn derive_key(
    password: &[u8],
    salt: &[u8],
    m_cost_kib: u32,
    t_cost: u32,
    p_cost: u32,
) -> Result<Zeroizing<[u8; KEY_LEN]>, Error> {
    let params = Params::new(m_cost_kib, t_cost, p_cost, Some(KEY_LEN))
        .map_err(|e| Error::Crypto(format!("argon2 params: {e}")))?;
    let argon = Argon2::new(Algorithm::Argon2id, Version::V0x13, params);
    let mut key = Zeroizing::new([0u8; KEY_LEN]);
    argon
        .hash_password_into(password, salt, key.as_mut())
        .map_err(|e| Error::Crypto(format!("argon2: {e}")))?;
    Ok(key)
}

pub fn seal(
    secret: &[u8],
    fingerprint: Fingerprint,
    network: Network,
    password: Password,
) -> Result<SecretEnvelope, Error> {
    seal_with_aad(secret, &associated_data(fingerprint, network)?, password)
}

/// Seals `secret` under `password`, bound to `aad`: an envelope opens only with the
/// same associated data.
pub fn seal_with_aad(
    secret: &[u8],
    aad: &[u8],
    password: Password,
) -> Result<SecretEnvelope, Error> {
    let mut salt = [0u8; SALT_LEN];
    getrandom::getrandom(&mut salt).map_err(|e| Error::Random(e.to_string()))?;
    let mut nonce = [0u8; NONCE_LEN];
    getrandom::getrandom(&mut nonce).map_err(|e| Error::Random(e.to_string()))?;

    let key = derive_key(password.0.as_bytes(), &salt, M_COST_KIB, T_COST, P_COST)?;

    let cipher = XChaCha20Poly1305::new(Key::from_slice(key.as_ref()));
    let ciphertext = cipher
        .encrypt(XNonce::from_slice(&nonce), Payload { msg: secret, aad })
        .map_err(|e| Error::Crypto(format!("encryption failed: {e}")))?;

    Ok(SecretEnvelope {
        version: SECRET_ENVELOPE_VERSION,
        kdf: KDF_NAME.to_string(),
        m_cost_kib: M_COST_KIB,
        t_cost: T_COST,
        p_cost: P_COST,
        salt: BASE64_STANDARD.encode(salt),
        cipher: CIPHER_NAME.to_string(),
        nonce: BASE64_STANDARD.encode(nonce),
        ciphertext: BASE64_STANDARD.encode(ciphertext),
    })
}

pub fn open(
    envelope: &SecretEnvelope,
    fingerprint: Fingerprint,
    network: Network,
    password: Password,
) -> Result<Zeroizing<Vec<u8>>, Error> {
    open_with_aad(envelope, &associated_data(fingerprint, network)?, password)
}

/// Opens an envelope sealed by `seal_with_aad` with the same `aad`.
pub fn open_with_aad(
    envelope: &SecretEnvelope,
    aad: &[u8],
    password: Password,
) -> Result<Zeroizing<Vec<u8>>, Error> {
    if envelope.version > SECRET_ENVELOPE_VERSION {
        return Err(Error::UnsupportedEnvelopeVersion {
            found: envelope.version,
            supported: SECRET_ENVELOPE_VERSION,
        });
    }
    if envelope.kdf != KDF_NAME {
        return Err(Error::UnsupportedKdf(envelope.kdf.clone()));
    }
    if envelope.cipher != CIPHER_NAME {
        return Err(Error::UnsupportedCipher(envelope.cipher.clone()));
    }
    if envelope.m_cost_kib < MIN_M_COST_KIB {
        return Err(Error::WeakKdfParams {
            field: "m_cost_kib",
            found: envelope.m_cost_kib,
            minimum: MIN_M_COST_KIB,
        });
    }
    if envelope.t_cost < MIN_T_COST {
        return Err(Error::WeakKdfParams {
            field: "t_cost",
            found: envelope.t_cost,
            minimum: MIN_T_COST,
        });
    }
    if envelope.p_cost < MIN_P_COST {
        return Err(Error::WeakKdfParams {
            field: "p_cost",
            found: envelope.p_cost,
            minimum: MIN_P_COST,
        });
    }

    let salt = BASE64_STANDARD
        .decode(&envelope.salt)
        .map_err(|e| Error::Base64(format!("salt: {e}")))?;
    let nonce = BASE64_STANDARD
        .decode(&envelope.nonce)
        .map_err(|e| Error::Base64(format!("nonce: {e}")))?;
    let ciphertext = BASE64_STANDARD
        .decode(&envelope.ciphertext)
        .map_err(|e| Error::Base64(format!("ciphertext: {e}")))?;

    if salt.len() != SALT_LEN {
        return Err(Error::Crypto(format!(
            "invalid salt length: expected {SALT_LEN}, found {}",
            salt.len()
        )));
    }
    if nonce.len() != NONCE_LEN {
        return Err(Error::Crypto(format!(
            "invalid nonce length: expected {NONCE_LEN}, found {}",
            nonce.len()
        )));
    }

    let key = derive_key(
        password.0.as_bytes(),
        &salt,
        envelope.m_cost_kib,
        envelope.t_cost,
        envelope.p_cost,
    )?;

    let cipher = XChaCha20Poly1305::new(Key::from_slice(key.as_ref()));
    let plaintext = cipher
        .decrypt(
            XNonce::from_slice(&nonce),
            Payload {
                msg: &ciphertext,
                aad,
            },
        )
        .map_err(|_| Error::WrongPassword)?;

    Ok(Zeroizing::new(plaintext))
}

/// Opens `record`'s envelope and decodes it to the mnemonic string, the one way to
/// go from a stored secret to a usable mnemonic. Both revealing and signing must
/// call this instead of re-implementing the open-then-decode sequence.
pub(crate) fn open_mnemonic(
    record: &KeyRecord,
    network: Network,
    password: Password,
) -> Result<Zeroizing<String>, Error> {
    let envelope = record
        .secret
        .as_ref()
        .ok_or(Error::NoSecret(record.fingerprint))?;
    let plaintext = open(envelope, record.fingerprint, network, password)?;
    let mnemonic = std::str::from_utf8(&plaintext).map_err(|e| Error::Mnemonic(e.to_string()))?;
    Ok(Zeroizing::new(mnemonic.to_string()))
}

#[cfg(test)]
mod tests {
    use bitcoin::Network;

    use super::*;

    fn fingerprint(value: u32) -> Fingerprint {
        Fingerprint::from(value.to_be_bytes())
    }

    fn password(value: &str) -> Password {
        Password::new(value.to_string())
    }

    #[test]
    fn seal_then_open_round_trips() {
        let secret = b"abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
        let envelope = seal(
            secret,
            fingerprint(1),
            Network::Signet,
            password("correct horse"),
        )
        .expect("seal succeeds");
        let opened = open(
            &envelope,
            fingerprint(1),
            Network::Signet,
            password("correct horse"),
        )
        .expect("open succeeds");
        assert_eq!(
            opened.as_slice(),
            secret,
            "round tripped secret does not match, got {opened:?}"
        );
    }

    #[test]
    fn open_with_a_wrong_password_errors() {
        let envelope = seal(
            b"secret",
            fingerprint(1),
            Network::Signet,
            password("right"),
        )
        .expect("seal succeeds");
        let result = open(
            &envelope,
            fingerprint(1),
            Network::Signet,
            password("wrong"),
        );
        assert!(
            matches!(result, Err(Error::WrongPassword)),
            "expected WrongPassword, got {result:?}"
        );
    }

    #[test]
    fn open_with_another_fingerprint_errors() {
        let envelope = seal(
            b"secret",
            fingerprint(1),
            Network::Signet,
            password("right"),
        )
        .expect("seal succeeds");
        let result = open(
            &envelope,
            fingerprint(2),
            Network::Signet,
            password("right"),
        );
        assert!(
            matches!(result, Err(Error::WrongPassword)),
            "expected WrongPassword for a mismatched fingerprint, got {result:?}"
        );
    }

    #[test]
    fn open_on_another_network_errors() {
        let envelope = seal(
            b"secret",
            fingerprint(1),
            Network::Signet,
            password("right"),
        )
        .expect("seal succeeds");
        let result = open(
            &envelope,
            fingerprint(1),
            Network::Regtest,
            password("right"),
        );
        assert!(
            matches!(result, Err(Error::WrongPassword)),
            "expected WrongPassword for a mismatched network, got {result:?}"
        );
    }

    #[test]
    fn envelope_records_the_format_literals() {
        let envelope = seal(
            b"secret",
            fingerprint(1),
            Network::Signet,
            password("right"),
        )
        .expect("seal succeeds");
        assert_eq!(envelope.kdf, "argon2id", "unexpected kdf: {}", envelope.kdf);
        assert_eq!(
            envelope.cipher, "xchacha20poly1305",
            "unexpected cipher: {}",
            envelope.cipher
        );
        assert_eq!(
            envelope.version, SECRET_ENVELOPE_VERSION,
            "unexpected version: {}",
            envelope.version
        );
        assert_eq!(
            envelope.m_cost_kib, 65_536,
            "unexpected m_cost_kib: {}",
            envelope.m_cost_kib
        );
        assert_eq!(envelope.t_cost, 3, "unexpected t_cost: {}", envelope.t_cost);
        assert_eq!(envelope.p_cost, 1, "unexpected p_cost: {}", envelope.p_cost);

        let salt = BASE64_STANDARD
            .decode(&envelope.salt)
            .expect("valid salt base64");
        assert_eq!(
            salt.len(),
            SALT_LEN,
            "unexpected salt length: {}",
            salt.len()
        );
        let nonce = BASE64_STANDARD
            .decode(&envelope.nonce)
            .expect("valid nonce base64");
        assert_eq!(
            nonce.len(),
            NONCE_LEN,
            "unexpected nonce length: {}",
            nonce.len()
        );
    }

    #[test]
    fn two_seals_of_the_same_secret_differ() {
        let first = seal(
            b"secret",
            fingerprint(1),
            Network::Signet,
            password("right"),
        )
        .expect("first seal succeeds");
        let second = seal(
            b"secret",
            fingerprint(1),
            Network::Signet,
            password("right"),
        )
        .expect("second seal succeeds");
        assert_ne!(first.salt, second.salt, "salts should differ between seals");
        assert_ne!(
            first.nonce, second.nonce,
            "nonces should differ between seals"
        );
        assert_ne!(
            first.ciphertext, second.ciphertext,
            "ciphertexts should differ between seals"
        );
    }

    #[test]
    fn weak_parameters_are_refused() {
        let mut envelope = seal(
            b"secret",
            fingerprint(1),
            Network::Signet,
            password("right"),
        )
        .expect("seal succeeds");

        envelope.m_cost_kib = MIN_M_COST_KIB - 1;
        let result = open(
            &envelope,
            fingerprint(1),
            Network::Signet,
            password("right"),
        );
        assert!(
            matches!(
                result,
                Err(Error::WeakKdfParams {
                    field: "m_cost_kib",
                    ..
                })
            ),
            "expected WeakKdfParams for m_cost_kib, got {result:?}"
        );
        envelope.m_cost_kib = MIN_M_COST_KIB;

        envelope.t_cost = MIN_T_COST - 1;
        let result = open(
            &envelope,
            fingerprint(1),
            Network::Signet,
            password("right"),
        );
        assert!(
            matches!(
                result,
                Err(Error::WeakKdfParams {
                    field: "t_cost",
                    ..
                })
            ),
            "expected WeakKdfParams for t_cost, got {result:?}"
        );
    }

    #[test]
    fn unknown_kdf_or_cipher_is_refused() {
        let mut envelope = seal(
            b"secret",
            fingerprint(1),
            Network::Signet,
            password("right"),
        )
        .expect("seal succeeds");

        envelope.kdf = "scrypt".to_string();
        let result = open(
            &envelope,
            fingerprint(1),
            Network::Signet,
            password("right"),
        );
        assert!(
            matches!(result, Err(Error::UnsupportedKdf(ref found)) if found == "scrypt"),
            "expected UnsupportedKdf, got {result:?}"
        );
        envelope.kdf = KDF_NAME.to_string();

        envelope.cipher = "aes-gcm".to_string();
        let result = open(
            &envelope,
            fingerprint(1),
            Network::Signet,
            password("right"),
        );
        assert!(
            matches!(result, Err(Error::UnsupportedCipher(ref found)) if found == "aes-gcm"),
            "expected UnsupportedCipher, got {result:?}"
        );
    }

    #[test]
    fn a_newer_envelope_version_is_refused() {
        let mut envelope = seal(
            b"secret",
            fingerprint(1),
            Network::Signet,
            password("right"),
        )
        .expect("seal succeeds");
        envelope.version = SECRET_ENVELOPE_VERSION + 1;
        let result = open(
            &envelope,
            fingerprint(1),
            Network::Signet,
            password("right"),
        );
        assert!(
            matches!(
                result,
                Err(Error::UnsupportedEnvelopeVersion { found, supported })
                    if found == SECRET_ENVELOPE_VERSION + 1 && supported == SECRET_ENVELOPE_VERSION
            ),
            "expected UnsupportedEnvelopeVersion, got {result:?}"
        );
    }

    #[test]
    fn a_corrupt_base64_field_is_refused() {
        let mut envelope = seal(
            b"secret",
            fingerprint(1),
            Network::Signet,
            password("right"),
        )
        .expect("seal succeeds");
        envelope.salt = "not base64!".to_string();
        let result = open(
            &envelope,
            fingerprint(1),
            Network::Signet,
            password("right"),
        );
        assert!(
            matches!(result, Err(Error::Base64(_))),
            "expected Base64, got {result:?}"
        );
    }
}
