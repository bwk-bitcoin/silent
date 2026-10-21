//! Transaction-independent public coin exports. Envelopes contain one input, no outputs.

use bitcoin::{absolute, relative, transaction, Psbt, Transaction};
use bwk::{
    bwk_coin::{Coin, CoinSourceKind, CoinSpendInfo, CoinStatus},
    bwk_descriptor::spend::{restrict_variant, spend_variants, Locks, Maturity, Preimage},
    bwk_tx::{bwk_psbt::PsbtV2, recipient::SpUpdater},
};
use bwk_sp::account::updater::SpInputUpdater;

use crate::ffi::{
    Network, RustSpendCoin, RustSpendLock, RustSpendVariant, SpendAvailability, SpendCoinStatus,
    SpendCoinsResult, SpendLockKind, SpendMaturity,
};

pub(crate) const SP_SATISFACTION_WU: u64 = 67;

impl From<CoinStatus> for SpendCoinStatus {
    fn from(status: CoinStatus) -> Self {
        match status {
            CoinStatus::Unconfirmed => Self::Unconfirmed,
            CoinStatus::ConfirmedUnverified => Self::ConfirmedUnverified,
            CoinStatus::Confirmed => Self::Confirmed,
            CoinStatus::BeingSpend => Self::BeingSpent,
            CoinStatus::Spent => Self::Spent,
        }
    }
}

impl From<Maturity> for SpendMaturity {
    fn from(maturity: Maturity) -> Self {
        match maturity {
            Maturity::Mature => Self::Mature,
            Maturity::Immature => Self::Immature,
            Maturity::Unknown => Self::Unknown,
        }
    }
}

impl From<Option<absolute::LockTime>> for RustSpendLock {
    fn from(lock: Option<absolute::LockTime>) -> Self {
        match lock {
            None => Self {
                kind: SpendLockKind::None,
                value: 0,
            },
            Some(absolute::LockTime::Blocks(height)) => Self {
                kind: SpendLockKind::AbsoluteHeight,
                value: height.to_consensus_u32(),
            },
            Some(absolute::LockTime::Seconds(time)) => Self {
                kind: SpendLockKind::AbsoluteTime,
                value: time.to_consensus_u32(),
            },
        }
    }
}

impl From<Option<relative::LockTime>> for RustSpendLock {
    fn from(lock: Option<relative::LockTime>) -> Self {
        match lock {
            None => Self {
                kind: SpendLockKind::None,
                value: 0,
            },
            Some(relative::LockTime::Blocks(blocks)) => Self {
                kind: SpendLockKind::RelativeBlocks,
                value: u32::from(blocks.value()),
            },
            Some(relative::LockTime::Time(time)) => Self {
                kind: SpendLockKind::RelativeTime512,
                value: u32::from(time.value()),
            },
        }
    }
}

impl From<Result<Vec<RustSpendCoin>, String>> for SpendCoinsResult {
    fn from(result: Result<Vec<RustSpendCoin>, String>) -> Self {
        match result {
            Ok(coins) => Self {
                is_ok: true,
                error: String::new(),
                coins,
            },
            Err(error) => Self {
                is_ok: false,
                error,
                coins: Vec::new(),
            },
        }
    }
}

fn envelope(coin: &Coin) -> Result<Psbt, String> {
    if !coin.txout.script_pubkey.is_witness_program() && !coin.txout.script_pubkey.is_p2sh() {
        return Err("unsupported legacy coin export: full previous transaction unavailable".into());
    }
    let tx = Transaction {
        version: transaction::Version::TWO,
        lock_time: absolute::LockTime::ZERO,
        input: vec![coin.clone().into()],
        output: Vec::new(),
    };
    let mut psbt = Psbt::from_unsigned_tx(tx).map_err(|e| e.to_string())?;
    psbt.inputs[0] = coin.to_psbt_input().map_err(|e| e.to_string())?;
    let witness = coin.txout.script_pubkey.is_witness_program()
        || psbt.inputs[0]
            .redeem_script
            .as_ref()
            .is_some_and(|script| script.is_witness_program());
    if !witness && psbt.inputs[0].non_witness_utxo.is_none() {
        return Err("unsupported legacy coin export: full previous transaction unavailable".into());
    }
    Ok(psbt)
}

fn encode(psbt: Psbt) -> Result<String, String> {
    PsbtV2::from_bitcoin_psbt(psbt)
        .and_then(|psbt| psbt.serialize())
        .map(hex::encode)
        .map_err(|e| e.to_string())
}

pub(crate) fn preimage_requirement(preimage: &Preimage) -> String {
    match preimage {
        Preimage::Sha256(hash) => format!("sha256:{hash}"),
        Preimage::Hash256(hash) => format!("hash256:{hash}"),
        Preimage::Ripemd160(hash) => format!("ripemd160:{hash}"),
        Preimage::Hash160(hash) => format!("hash160:{hash}"),
    }
}

pub(crate) fn descriptor_coin(coin: &Coin, network: Network) -> Result<RustSpendCoin, String> {
    let CoinSpendInfo::Bip32 {
        coin_path: (keychain, index),
        descriptor,
    } = &coin.spend_info
    else {
        return Err("descriptor export requires a BIP32 coin".into());
    };
    let keychain = u32::from(*keychain) as usize;
    let exported = spend_variants(descriptor, keychain, *index).map_err(|e| e.to_string())?;
    let template = envelope(coin)?;
    let mut variants = Vec::with_capacity(exported.variants.len());
    for variant in exported.variants {
        let mut psbt = template.clone();
        restrict_variant(descriptor, keychain, *index, &variant.id, &mut psbt, 0)
            .map_err(|e| e.to_string())?;
        let locks = variant.requirements.locks;
        variants.push(RustSpendVariant {
            path_id: variant.id.to_string(),
            keychain: keychain as u32,
            derivation_index: *index,
            display: format!(
                "{:?}, alternative {}",
                variant.id.path, variant.id.alternative
            ),
            psbt_v2_hex: encode(psbt)?,
            descriptor: exported.descriptor.to_string(),
            sp_spend_key: String::new(),
            required_keys: variant
                .requirements
                .keys
                .iter()
                .map(ToString::to_string)
                .collect(),
            required_preimages: variant
                .requirements
                .preimages
                .iter()
                .map(preimage_requirement)
                .collect(),
            satisfaction_wu: variant.satisfaction_wu,
            absolute_lock: locks.absolute.into(),
            relative_lock: locks.relative.into(),
            // Scanner exposes no coherent tip/hash/MTP snapshot; do not invent one.
            maturity: locks.maturity(None).map_err(|e| e.to_string())?.into(),
            availability: SpendAvailability::Unknown,
        });
    }
    Ok(public_coin(coin, network, variants))
}

pub(crate) fn sp_coin(
    coin: &Coin,
    network: Network,
    spend_pk: bitcoin::secp256k1::PublicKey,
    source: Option<bitcoin::bip32::KeySource>,
) -> Result<RustSpendCoin, String> {
    let mut psbt = PsbtV2::from_bitcoin_psbt(envelope(coin)?).map_err(|e| e.to_string())?;
    SpInputUpdater::new(spend_pk, source.clone())
        .update_sp_inputs(&mut psbt, std::slice::from_ref(coin))
        .map_err(|e| format!("{e:?}"))?;
    // Public spend-key identity only, never the scan secret in an sp() descriptor.
    let policy = format!("sp-spend({spend_pk})");
    let key = match source {
        Some((fingerprint, path)) => format!("[{fingerprint}/{path}]{spend_pk}"),
        None => spend_pk.to_string(),
    };
    let locks = Locks::default();
    let variants = vec![RustSpendVariant {
        path_id: format!("{policy}:Key:0"),
        keychain: 0,
        derivation_index: 0,
        display: "Silent Payment key path".into(),
        psbt_v2_hex: hex::encode(psbt.serialize().map_err(|e| e.to_string())?),
        descriptor: String::new(),
        sp_spend_key: spend_pk.to_string(),
        required_keys: vec![key],
        required_preimages: Vec::new(),
        // Witness count, signature length, 65-byte signature (non-default sighash).
        satisfaction_wu: SP_SATISFACTION_WU,
        absolute_lock: locks.absolute.into(),
        relative_lock: locks.relative.into(),
        maturity: SpendMaturity::Mature,
        availability: SpendAvailability::Unknown,
    }];
    Ok(public_coin(coin, network, variants))
}

pub(crate) fn set_availability(coin: &mut RustSpendCoin, coinbase: Option<bool>) {
    for variant in &mut coin.variants {
        variant.availability = match (coin.status, variant.maturity, coinbase) {
            (SpendCoinStatus::Spent | SpendCoinStatus::BeingSpent, _, _) => {
                SpendAvailability::Spent
            }
            (_, SpendMaturity::Immature, _) => SpendAvailability::Immature,
            (_, SpendMaturity::Mature, Some(false)) => SpendAvailability::Available,
            _ => SpendAvailability::Unknown,
        };
    }
}

fn public_coin(coin: &Coin, network: Network, variants: Vec<RustSpendVariant>) -> RustSpendCoin {
    let mut exported = RustSpendCoin {
        outpoint: coin.outpoint.to_string(),
        account_type: match coin.source() {
            CoinSourceKind::SilentPayment => "SP",
            CoinSourceKind::Segwit => "Segwit",
            CoinSourceKind::Taproot => "Taproot",
            CoinSourceKind::Other => "Other",
        }
        .into(),
        network,
        value: coin.value().to_sat(),
        height: coin.height.unwrap_or(0),
        has_height: coin.height.is_some(),
        label: coin.label.clone().unwrap_or_default(),
        status: coin.status.into(),
        variants,
    };
    set_availability(&mut exported, None);
    exported
}

#[cfg(test)]
mod tests {
    use std::str::FromStr;

    use bitcoin::{
        bip32::{DerivationPath, Xpriv, Xpub},
        hashes::Hash,
        secp256k1::{PublicKey, Secp256k1, SecretKey},
        Amount, Network as BitcoinNetwork, OutPoint, Sequence, TxOut, Txid,
    };
    use bwk::{
        bwk_coin::{derive_descriptor, Coin, CoinSpendInfo, CoinStatus, KeyChain},
        bwk_descriptor::{spend::Locks, wallet::WalletDescriptor},
        bwk_tx::bwk_psbt::{Output, PsbtV2},
    };

    use crate::{
        coordination::coordination_prepare,
        ffi::{
            Network, RustSelectedInput, SigningStage, SpendCoinStatus, SpendError, SpendLockKind,
            SpendMaturity,
        },
        spending::{descriptor_coin, sp_coin},
    };

    fn key(seed: u8) -> String {
        let secp = Secp256k1::new();
        let private = Xpriv::new_master(BitcoinNetwork::Regtest, &[seed; 32]).expect("master");
        format!(
            "[{}]{}/<0;1>/*",
            private.fingerprint(&secp),
            Xpub::from_priv(&secp, &private)
        )
    }

    fn coin(text: &str, index: u32) -> Coin {
        let descriptor = WalletDescriptor::from_str(text).expect("test descriptor");
        let derived =
            derive_descriptor(&descriptor, KeyChain::Receive, index).expect("derive coin");
        Coin {
            txout: TxOut {
                value: Amount::from_sat(100_000),
                script_pubkey: derived.script_pubkey(),
            },
            outpoint: OutPoint {
                txid: Txid::from_byte_array([1; 32]),
                vout: index,
            },
            height: Some(100),
            sequence: Sequence::MAX,
            status: CoinStatus::Confirmed,
            label: Some("exported coin".into()),
            satisfaction_size: 0,
            spend_info: CoinSpendInfo::Bip32 {
                coin_path: (KeyChain::Receive, index),
                descriptor,
            },
        }
    }

    #[test]
    fn exports_independent_stable_paths_and_clean_v2_envelopes() {
        let text = format!("tr({},and_v(v:pk({}),older(12)))", key(1), key(2));
        let first = descriptor_coin(&coin(&text, 0), Network::Regtest).expect("first export");
        let second = descriptor_coin(&coin(&text, 7), Network::Regtest).expect("second export");
        assert_eq!(
            first.variants.len(),
            2,
            "key and delayed leaf are both exported"
        );
        assert_eq!(
            first.status,
            SpendCoinStatus::Confirmed,
            "confirmation status retained"
        );
        assert_eq!(first.label, "exported coin", "label retained");
        for (a, b) in first.variants.iter().zip(&second.variants) {
            assert_eq!(
                a.path_id, b.path_id,
                "path identity must survive derivation"
            );
            assert_ne!(
                a.psbt_v2_hex, b.psbt_v2_hex,
                "prevout and derivations differ"
            );
            let bytes = hex::decode(&a.psbt_v2_hex).expect("hex envelope");
            let v2 = PsbtV2::deserialize(&bytes).expect("PSBTv2 envelope");
            assert_eq!(v2.inputs.len(), 1, "exactly one input");
            assert!(v2.outputs.is_empty(), "no transaction-dependent outputs");
            let v0 = v2.into_bitcoin_psbt().expect("Bitcoin metadata round trip");
            assert_eq!(
                v0.inputs[0]
                    .witness_utxo
                    .as_ref()
                    .expect("prevout")
                    .value
                    .to_sat(),
                100_000
            );
            assert!(v0.inputs[0].partial_sigs.is_empty(), "no ECDSA signatures");
            assert!(v0.inputs[0].tap_key_sig.is_none(), "no key signature");
            assert!(
                v0.inputs[0].tap_script_sigs.is_empty(),
                "no leaf signatures"
            );
            assert!(v0.proprietary.is_empty(), "no attempt identity");
            assert!(!a.descriptor.is_empty(), "full policy context");
            assert!(a.sp_spend_key.is_empty(), "not an SP policy");
        }
        assert_eq!(
            first.variants[0].satisfaction_wu, 67,
            "conservative key-path weight"
        );
        assert_eq!(
            first.variants[0].maturity,
            SpendMaturity::Mature,
            "no timelock"
        );
        assert_eq!(
            first.variants[1].relative_lock.kind,
            SpendLockKind::RelativeBlocks
        );
        assert_eq!(first.variants[1].relative_lock.value, 12);
        assert_eq!(
            first.variants[1].maturity,
            SpendMaturity::Unknown,
            "no coherent tip available"
        );
    }

    #[test]
    fn exports_alternative_signers_and_time_locks_without_cartesian_products() {
        let text = format!(
            "tr({},or_d(pk({}),and_v(v:pk({}),older(4194306))))",
            key(1),
            key(2),
            key(3)
        );
        let exported = descriptor_coin(&coin(&text, 0), Network::Signet).expect("alternatives");
        assert_eq!(
            exported.variants.len(),
            3,
            "key path plus two leaf satisfactions"
        );
        let timed = exported
            .variants
            .iter()
            .find(|v| v.relative_lock.kind == SpendLockKind::RelativeTime512)
            .expect("time alternative");
        assert_eq!(
            timed.relative_lock.value, 2,
            "two 512-second units, not seconds or sequence bits"
        );
        assert_eq!(
            timed.required_keys.len(),
            1,
            "only this alternative's signer"
        );
        assert_eq!(exported.network, Network::Signet, "network is explicit");
        let mut envelope =
            PsbtV2::deserialize(&hex::decode(&timed.psbt_v2_hex).expect("template hex"))
                .expect("template");
        envelope.outputs.push(Output {
            amount: Amount::from_sat(90_000),
            script_pubkey: Some(coin(&text, 0).txout.script_pubkey),
            psbt: bitcoin::psbt::Output::default(),
        });
        let result = coordination_prepare(
            Network::Signet,
            &hex::encode(envelope.serialize().expect("PSBT")),
            vec![RustSelectedInput {
                input_index: 0,
                network: Network::Signet,
                variant: timed.clone(),
            }],
            SigningStage::Signatures,
        );
        assert_eq!(
            result.kind,
            SpendError::Unsupported,
            "alternative remains exported but unsupported signing is explicit"
        );
    }

    #[test]
    fn exports_sp_public_metadata_without_scan_secret() {
        let secp = Secp256k1::new();
        let spend_pk =
            PublicKey::from_secret_key(&secp, &SecretKey::from_slice(&[2; 32]).expect("spend key"));
        let input = mock_sp_input(spend_pk, 0);
        let source = (
            [1, 2, 3, 4].into(),
            DerivationPath::from_str("m/352'/1'/0'/1'/0").expect("SP path"),
        );
        let exported =
            sp_coin(&input, Network::Regtest, spend_pk, Some(source.clone())).expect("SP export");
        let variant = &exported.variants[0];
        assert!(
            variant.descriptor.is_empty(),
            "never export the private scan descriptor"
        );
        assert_eq!(
            variant.sp_spend_key,
            spend_pk.to_string(),
            "public SP policy context"
        );
        let psbt = PsbtV2::deserialize(&hex::decode(&variant.psbt_v2_hex).expect("hex"))
            .expect("SP envelope");
        let expected = input.to_psbt_input().expect("upstream SP input");
        for (key, value) in expected.unknown {
            assert_eq!(
                psbt.inputs[0].psbt.unknown.get(&key),
                Some(&value),
                "preserve SP proprietary input metadata"
            );
        }
        assert_eq!(variant.satisfaction_wu, 67, "SP schnorr witness bound");
    }

    fn mock_sp_input(spend_pk: PublicKey, index: u32) -> Coin {
        let mut input = coin(&format!("tr({})", key(1)), index);
        input.spend_info = CoinSpendInfo::Sp { tweak: [3; 32] };
        let secp = Secp256k1::new();
        let output_key = spend_pk
            .add_exp_tweak(
                &secp,
                &bitcoin::secp256k1::Scalar::from_be_bytes([3; 32]).expect("SP tweak"),
            )
            .expect("SP output key")
            .x_only_public_key()
            .0;
        input.txout.script_pubkey = bitcoin::ScriptBuf::new_p2tr_tweaked(
            bitcoin::key::TweakedPublicKey::dangerous_assume_tweaked(output_key),
        );
        input
    }

    #[test]
    fn mock_two_sp_owner_combination_is_explicitly_unsupported() {
        let mut selected = Vec::new();
        for index in 0..2 {
            let spend_pk = PublicKey::from_secret_key(
                &Secp256k1::new(),
                &SecretKey::from_slice(&[index as u8 + 1; 32]).expect("mock key"),
            );
            let input = mock_sp_input(spend_pk, index);
            let exported = sp_coin(&input, Network::Regtest, spend_pk, None).expect("mock export");
            selected.push(RustSelectedInput {
                input_index: index,
                network: Network::Regtest,
                variant: exported.variants[0].clone(),
            });
        }
        let mut psbt =
            PsbtV2::deserialize(&hex::decode(&selected[0].variant.psbt_v2_hex).expect("hex"))
                .expect("first envelope");
        let second =
            PsbtV2::deserialize(&hex::decode(&selected[1].variant.psbt_v2_hex).expect("hex"))
                .expect("second envelope");
        psbt.inputs.extend(second.inputs);
        psbt.outputs.push(Output {
            amount: Amount::from_sat(190_000),
            script_pubkey: psbt.inputs[0]
                .psbt
                .witness_utxo
                .as_ref()
                .map(|utxo| utxo.script_pubkey.clone()),
            psbt: bitcoin::psbt::Output::default(),
        });
        let result = coordination_prepare(
            Network::Regtest,
            &hex::encode(psbt.serialize().expect("mock PSBT")),
            selected,
            SigningStage::Signatures,
        );
        assert_eq!(
            result.kind,
            SpendError::Unsupported,
            "multiple owners are representable but current backend refuses the combination"
        );
    }

    #[test]
    fn rejects_descriptor_prevout_mismatch() {
        let mut input = coin(&format!("tr({})", key(1)), 0);
        input.txout.script_pubkey = coin(&format!("tr({})", key(2)), 0).txout.script_pubkey;
        assert!(
            descriptor_coin(&input, Network::Regtest).is_err(),
            "mismatched prevout must fail, not export a false policy"
        );
    }

    #[test]
    fn exports_segwit_but_refuses_incomplete_legacy_metadata() {
        for text in [format!("wpkh({})", key(1)), format!("sh(wpkh({}))", key(1))] {
            let exported =
                descriptor_coin(&coin(&text, 0), Network::Regtest).expect("segwit export");
            assert_eq!(exported.variants.len(), 1, "one segwit satisfaction");
            assert!(
                exported.variants[0].satisfaction_wu > 67,
                "ECDSA and pubkey witness"
            );
        }
        let result = descriptor_coin(&coin(&format!("pkh({})", key(1)), 0), Network::Regtest);
        assert_eq!(
            result.expect_err("legacy requires full previous transaction"),
            "unsupported legacy coin export: full previous transaction unavailable"
        );
    }

    #[test]
    fn lock_units_remain_distinct() {
        let height = Locks {
            absolute: Some(bitcoin::absolute::LockTime::from_consensus(100)),
            relative: None,
        };
        let time = Locks {
            absolute: Some(bitcoin::absolute::LockTime::from_consensus(500_000_100)),
            relative: None,
        };
        assert!(
            height.merge(time).is_err(),
            "height/time requirements cannot share nLockTime"
        );
        let exported: crate::ffi::RustSpendLock = time.absolute.into();
        assert_eq!(exported.kind, SpendLockKind::AbsoluteTime);
        assert_eq!(exported.value, 500_000_100);
    }

    #[test]
    fn selected_hash_requirements_accept_only_matching_preimages() {
        use bitcoin::hashes::{hash160, ripemd160, sha256, sha256d};
        let preimage = vec![9; 32];
        for kind in ["sha256", "hash256", "hash160", "ripemd160"] {
            let digest = match kind {
                "sha256" => hex::encode(sha256::Hash::hash(&preimage).to_byte_array()),
                "hash256" => hex::encode(sha256d::Hash::hash(&preimage).to_byte_array()),
                "hash160" => hex::encode(hash160::Hash::hash(&preimage).to_byte_array()),
                "ripemd160" => hex::encode(ripemd160::Hash::hash(&preimage).to_byte_array()),
                _ => unreachable!("fixed test kinds"),
            };
            let coin = coin(&format!("wsh(and_v(v:pk({}),{kind}({digest})))", key(1)), 0);
            let exported =
                descriptor_coin(&coin, Network::Regtest).expect("hash requirement export");
            let variant = exported.variants[0].clone();
            assert_eq!(
                variant.required_preimages,
                vec![format!("{kind}:{digest}")],
                "typed hash requirement"
            );
            let mut psbt = PsbtV2::deserialize(&hex::decode(&variant.psbt_v2_hex).expect("hex"))
                .expect("template");
            psbt.outputs.push(Output {
                amount: Amount::from_sat(90_000),
                script_pubkey: Some(coin.txout.script_pubkey),
                psbt: bitcoin::psbt::Output::default(),
            });
            let input = &mut psbt.inputs[0].psbt;
            match kind {
                "sha256" => {
                    input
                        .sha256_preimages
                        .insert(sha256::Hash::hash(&preimage), preimage.clone());
                }
                "hash256" => {
                    input
                        .hash256_preimages
                        .insert(sha256d::Hash::hash(&preimage), preimage.clone());
                }
                "hash160" => {
                    input
                        .hash160_preimages
                        .insert(hash160::Hash::hash(&preimage), preimage.clone());
                }
                "ripemd160" => {
                    input
                        .ripemd160_preimages
                        .insert(ripemd160::Hash::hash(&preimage), preimage.clone());
                }
                _ => unreachable!("fixed test kinds"),
            }
            let selected = vec![RustSelectedInput {
                input_index: 0,
                network: Network::Regtest,
                variant,
            }];
            let accepted = coordination_prepare(
                Network::Regtest,
                &hex::encode(psbt.serialize().expect("PSBT")),
                selected.clone(),
                SigningStage::Signatures,
            );
            assert!(accepted.is_ok, "valid {kind} preimage: {}", accepted.error);
            psbt.inputs[0]
                .psbt
                .sha256_preimages
                .insert(sha256::Hash::hash(&preimage), vec![0; 32]);
            let rejected = coordination_prepare(
                Network::Regtest,
                &hex::encode(psbt.serialize().expect("PSBT")),
                selected,
                SigningStage::Signatures,
            );
            assert!(
                !rejected.is_ok,
                "incorrect or unrequested preimage refused for {kind}"
            );
        }
    }
}
