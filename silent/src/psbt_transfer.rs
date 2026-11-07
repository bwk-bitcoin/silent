//! PSBT export and import as base64 text, the form other wallets read and write.

use base64::{engine::general_purpose::STANDARD, Engine};
use bwk_sp::bwk_tx::bwk_psbt::{Error as PsbtV2Error, PsbtV2};

use crate::ffi::PsbtExport;

/// The magic a binary PSBT file starts with (BIP174).
const PSBT_MAGIC: &[u8] = b"psbt\xff";

/// `psbt`, a serialized PSBTv2 as the app signs it, as base64, with its txid: none
/// while a silent payment output has no script yet.
pub fn export_psbt(psbt: &[u8]) -> PsbtExport {
    let txid = PsbtV2::deserialize(psbt).and_then(|parsed| match parsed.unsigned_tx() {
        Ok(tx) => Ok(tx.compute_txid().to_string()),
        Err(PsbtV2Error::MissingOutputScript) => Ok(String::new()),
        Err(e) => Err(e),
    });
    match txid {
        Ok(txid) => PsbtExport {
            is_ok: true,
            error: String::new(),
            base64: STANDARD.encode(psbt),
            txid,
        },
        Err(e) => PsbtExport {
            error: format!("Invalid PSBT: {e:?}"),
            ..PsbtExport::default()
        },
    }
}

/// The PSBT `text` holds: base64 (surrounding whitespace ignored), or a binary PSBT. A
/// version 0 PSBT is converted to version 2, the version the app signs.
pub(crate) fn decode_psbt_text(text: &[u8]) -> Result<PsbtV2, String> {
    let bytes = if text.starts_with(PSBT_MAGIC) {
        text.to_vec()
    } else {
        let text = std::str::from_utf8(text)
            .map_err(|_| "The PSBT is neither base64 text nor a binary PSBT".to_string())?;
        STANDARD
            .decode(text.trim())
            .map_err(|e| format!("The PSBT is not valid base64: {e}"))?
    };
    if !bytes.starts_with(PSBT_MAGIC) {
        return Err("The text is not a PSBT".into());
    }
    match PsbtV2::deserialize(&bytes) {
        Ok(psbt) => Ok(psbt),
        Err(v2) => match bitcoin::Psbt::deserialize(&bytes) {
            Ok(v0) => PsbtV2::from_bitcoin_psbt(v0).map_err(|e| format!("Invalid PSBT: {e:?}")),
            Err(_) => Err(format!("Invalid PSBT: {v2:?}")),
        },
    }
}

#[cfg(test)]
mod tests {
    use base64::{engine::general_purpose::STANDARD, Engine};

    use bitcoin::{
        absolute::LockTime, hashes::Hash, transaction::Version, Amount, OutPoint, Psbt, ScriptBuf,
        Transaction, TxIn, TxOut, Txid,
    };
    use bwk_sp::bwk_tx::bwk_psbt::PsbtV2;

    use crate::psbt_transfer::{decode_psbt_text, export_psbt};

    fn unsigned_v0() -> Psbt {
        Psbt::from_unsigned_tx(Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: vec![TxIn {
                previous_output: OutPoint::new(Txid::from_byte_array([5; 32]), 1),
                ..Default::default()
            }],
            output: vec![TxOut {
                value: Amount::from_sat(1_000),
                script_pubkey: ScriptBuf::new_op_return([1, 2, 3]),
            }],
        })
        .expect("unsigned transaction")
    }

    #[test]
    fn reads_base64_binary_and_version_0_psbts() {
        let v0 = unsigned_v0();
        let txid = v0.unsigned_tx.compute_txid();
        let v2 = PsbtV2::from_bitcoin_psbt(v0.clone())
            .and_then(|psbt| psbt.serialize())
            .expect("PSBTv2 bytes");
        let exported = export_psbt(&v2);
        assert!(exported.is_ok, "export: {}", exported.error);
        assert_eq!(exported.txid, txid.to_string(), "the export names its txid");
        assert_eq!(exported.base64, STANDARD.encode(&v2), "base64 of the bytes");
        let texts = [
            format!(" {}\n", exported.base64).into_bytes(),
            v2.clone(),
            STANDARD.encode(v0.serialize()).into_bytes(),
            v0.serialize(),
        ];
        for text in texts {
            let decoded = decode_psbt_text(&text).expect("a PSBT");
            assert_eq!(
                decoded.unsigned_tx().expect("unsigned tx").compute_txid(),
                txid,
                "every form reads as the same transaction"
            );
            assert_eq!(
                decoded.serialize().expect("serialize"),
                v2,
                "as the PSBTv2 the app signs"
            );
        }
        assert!(
            !export_psbt(&v0.serialize()).is_ok,
            "only a PSBTv2 is exported"
        );
    }

    #[test]
    fn refuses_text_that_is_no_psbt() {
        for text in [&b"not base64 !"[..], b"aGVsbG8=", b"", b"psbt\xff\x00"] {
            assert!(
                decode_psbt_text(text).is_err(),
                "{text:?} is refused as a PSBT"
            );
        }
        assert!(
            decode_psbt_text(STANDARD.encode(b"psbt\xff\x00").as_bytes()).is_err(),
            "a truncated base64 PSBT is refused"
        );
    }
}
