//! Firmware-backed MuSig2 nonce-session redemption. See ledger_nonce_redemption.md.

use std::{env, net::SocketAddr, str::FromStr};

use bitcoin::{
    absolute::LockTime,
    bip32::{DerivationPath, Fingerprint},
    hashes::Hash,
    psbt::{raw::ProprietaryKey, Psbt},
    secp256k1::{Secp256k1, SecretKey},
    transaction::Version,
    Amount, Network, OutPoint, ScriptBuf, Sequence, Transaction, TxIn, TxOut, Txid, Witness,
};
use bwk::{
    bwk_descriptor::{
        musig::parse_musig_descriptor,
        updater::update_input,
        wallet::{WalletDescriptor, WalletDescriptorExt},
    },
    bwk_sign::{
        hot_signer::HotSigner,
        musig::{aggregate_partial_sigs, musig_status, MusigRound},
    },
    bwk_tx::bwk_psbt::musig::{musig_input_partial_sigs, musig_input_pub_nonces},
};
use bwk_hwi::{ledger::LedgerSimulator, DeviceWallet, Error, HWI};

const HOT_MNEMONIC: &str =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
const COIN_SATS: u64 = 100_000;
const FEE_SATS: u64 = 1_000;
const OWNED_EXPRESSION_COUNT: usize = 3;

struct Setup {
    device: LedgerSimulator,
    wallet: DeviceWallet,
    descriptor: WalletDescriptor,
    fingerprint: Fingerprint,
}

impl Setup {
    fn connect() -> Self {
        let address: SocketAddr = env::var("LEDGER_SPECULOS_ADDR")
            .unwrap_or_else(|_| "127.0.0.1:9999".into())
            .parse()
            .expect("LEDGER_SPECULOS_ADDR must be a socket address");
        let device =
            LedgerSimulator::try_connect_on(address).expect("connect to Speculos firmware");
        let fingerprint = device
            .get_master_fingerprint()
            .expect("read Ledger fingerprint");
        assert_eq!(
            fingerprint.to_string(),
            "b8688df1",
            "Speculos must use the documented legal winner seed"
        );
        let path = DerivationPath::from_str("m/87'/1'/0'").unwrap();
        let ledger_xpub = device
            .get_extended_pubkey(&path)
            .expect("export Ledger account key");
        let hot = hot_signer();
        let hot_xpub = hot.xpub(&path);
        let keys: Vec<String> = (0..OWNED_EXPRESSION_COUNT)
            .map(|role| {
                let role_path = DerivationPath::from_str(&format!("m/{role}")).unwrap();
                let secp = Secp256k1::verification_only();
                let ledger_role = ledger_xpub.derive_pub(&secp, &role_path).unwrap();
                let hot_role = hot_xpub.xkey.derive_pub(&secp, &role_path).unwrap();
                format!(
                    "[{fingerprint}/87'/1'/0'/{role}]{ledger_role},[{}/87'/1'/0'/{role}]{hot_role}",
                    hot.fingerprint()
                )
            })
            .collect();
        let policy = format!(
            "tr(musig({})/<0;1>/*,and_v(v:pk(musig({})/<2;3>/*),pk(musig({})/<4;5>/*)))",
            keys[0], keys[1], keys[2]
        );
        let descriptor = parse_musig_descriptor(&policy).expect("parse three-expression policy");
        let name = "nonce-redemption";
        let hmac = device
            .register_wallet(name, &policy)
            .expect("approve policy registration");
        assert!(hmac.is_some(), "Ledger registration must return an HMAC");
        Self {
            device,
            wallet: DeviceWallet {
                name: name.into(),
                policy,
                hmac,
            },
            descriptor,
            fingerprint,
        }
    }

    fn round1(&self, inputs: usize, id: [u8; 32]) -> Psbt {
        let mut psbt = self.spend(inputs, id);
        self.device
            .sign_tx_with_wallet(&mut psbt, &self.wallet)
            .expect("firmware round 1 must generate nonces without approval");
        for input in &psbt.inputs {
            assert_eq!(
                musig_input_pub_nonces(input).unwrap().len(),
                OWNED_EXPRESSION_COUNT,
                "round 1 must prefetch all three owned expressions per input"
            );
            assert!(
                musig_input_partial_sigs(input).unwrap().is_empty(),
                "round 1 must not yield partial signatures"
            );
        }
        psbt
    }

    fn spend(&self, inputs: usize, id: [u8; 32]) -> Psbt {
        let secp = Secp256k1::new();
        let destination = SecretKey::from_slice(&[9; 32])
            .unwrap()
            .x_only_public_key(&secp)
            .0;
        let tx = Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: (0..inputs)
                .map(|index| TxIn {
                    previous_output: OutPoint {
                        txid: Txid::from_byte_array([42; 32]),
                        vout: index as u32,
                    },
                    script_sig: ScriptBuf::new(),
                    sequence: Sequence::ENABLE_RBF_NO_LOCKTIME,
                    witness: Witness::new(),
                })
                .collect(),
            output: vec![TxOut {
                value: Amount::from_sat(COIN_SATS * inputs as u64 - FEE_SATS),
                script_pubkey: ScriptBuf::new_p2tr(&secp, destination, None),
            }],
        };
        let mut psbt = Psbt::from_unsigned_tx(tx).unwrap();
        psbt.proprietary.insert(
            ProprietaryKey {
                prefix: b"MUSIG2_SESSION_ID".to_vec(),
                subtype: 0,
                key: vec![],
            },
            id.to_vec(),
        );
        let receive = self.descriptor.single_paths().unwrap().remove(0);
        for index in 0..inputs {
            let definite = receive.derive_at(index as u32).unwrap();
            psbt.inputs[index].witness_utxo = Some(TxOut {
                value: Amount::from_sat(COIN_SATS),
                script_pubkey: definite.script_pubkey(),
            });
            update_input(&mut psbt, index, &definite).expect("fill BIP-373 input metadata");
        }
        psbt
    }

    fn request(&self, prefetched: &Psbt, inputs: usize) -> Psbt {
        let mut psbt = prefetched.clone();
        psbt.inputs.truncate(inputs);
        psbt.unsigned_tx.input.truncate(inputs);
        psbt.unsigned_tx.output[0].value = Amount::from_sat(COIN_SATS * inputs as u64 - FEE_SATS);
        psbt
    }

    fn redeem_and_verify(&self, mut psbt: Psbt) -> Psbt {
        let status = musig_status(&psbt, &self.descriptor).unwrap();
        let ledger = status
            .cosigners
            .iter()
            .find(|signer| signer.fingerprint == self.fingerprint)
            .unwrap();
        assert_eq!(
            ledger.expected as usize,
            psbt.inputs.len() * OWNED_EXPRESSION_COUNT,
            "request must include all three owned expressions per input"
        );
        let hot = hot_signer();
        hot.inner_sign(&mut psbt, &self.descriptor)
            .expect("hot round 1");
        assert_eq!(
            musig_status(&psbt, &self.descriptor).unwrap().round,
            MusigRound::PartialSigs,
            "all requested slots must have both participants' real nonces"
        );
        let replay = psbt.clone();
        self.device
            .sign_tx_with_wallet(&mut psbt, &self.wallet)
            .expect("approve first redemption on firmware");
        for input in &psbt.inputs {
            let signatures = musig_input_partial_sigs(input).unwrap();
            assert_eq!(
                signatures.len(),
                OWNED_EXPRESSION_COUNT,
                "firmware must sign every requested owned expression"
            );
            assert!(
                signatures.iter().all(|sig| {
                    input
                        .tap_key_origins
                        .get(&sig.participant.x_only_public_key().0)
                        .is_some_and(|(_, (fingerprint, _))| *fingerprint == self.fingerprint)
                }),
                "first partial signatures must belong to Ledger"
            );
        }
        hot.inner_sign(&mut psbt, &self.descriptor)
            .expect("hot round 2");
        assert_eq!(
            musig_status(&psbt, &self.descriptor).unwrap().round,
            MusigRound::Ready,
            "both signers must complete every requested slot"
        );
        assert_eq!(
            aggregate_partial_sigs(&mut psbt, &self.descriptor)
                .expect("verify every partial and aggregate Schnorr signature"),
            psbt.inputs.len() * OWNED_EXPRESSION_COUNT,
            "verify all requested signatures"
        );
        replay
    }

    fn assert_consumed(&self, mut psbt: Psbt) {
        let before = psbt.clone();
        let result = self.device.sign_tx_with_wallet(&mut psbt, &self.wallet);
        assert!(matches!(result, Err(Error::MusigSession)),
            "consumed session must return firmware SW_BAD_STATE mapped to MusigSession, got {result:?}");
        assert_eq!(
            psbt, before,
            "denied redemption must leave the entire PSBT unchanged"
        );
        assert!(
            psbt.inputs
                .iter()
                .all(|input| musig_input_partial_sigs(input).unwrap().is_empty()),
            "denied redemption must yield no partial signatures"
        );
    }
}

fn hot_signer() -> HotSigner {
    HotSigner::new_from_mnemonics(Network::Testnet, HOT_MNEMONIC).unwrap()
}

fn session_id() -> [u8; 32] {
    let mut id = [0; 32];
    getrandom::getrandom(&mut id).expect("fresh external session ID");
    id
}

#[test]
#[ignore = "requires pinned Ledger firmware on Speculos and approval of device prompts"]
fn same_psbt_replay_and_fresh_round1_with_same_id() {
    let setup = Setup::connect();
    let id = session_id();
    let prefetched = setup.round1(1, id);
    let request = setup.request(&prefetched, 1);
    let replay = setup.redeem_and_verify(request);
    setup.assert_consumed(replay);
    let refreshed = setup.round1(1, id);
    assert_ne!(
        musig_input_pub_nonces(&prefetched.inputs[0]).unwrap(),
        musig_input_pub_nonces(&refreshed.inputs[0]).unwrap(),
        "fresh round 1 with the same external ID must replace the nonce root"
    );
    let replay = setup.redeem_and_verify(setup.request(&refreshed, 1));
    setup.assert_consumed(replay);
}

#[test]
#[ignore = "requires pinned Ledger firmware on Speculos and approval of device prompts"]
fn changed_transaction_cannot_redeem_consumed_session() {
    let setup = Setup::connect();
    let prefetched = setup.round1(1, session_id());
    let mut replay = setup.redeem_and_verify(setup.request(&prefetched, 1));
    replay.unsigned_tx.output[0].value -= Amount::from_sat(1);
    setup.assert_consumed(replay);
}

#[test]
#[ignore = "requires pinned Ledger firmware on Speculos and approval of device prompts"]
fn input_count_changes_cannot_redeem_consumed_session() {
    let setup = Setup::connect();
    for first in [1, 2, 8] {
        for second in [1, 2, 8] {
            eprintln!("input count: first={first}, replay={second}");
            let prefetched = setup.round1(first.max(second), session_id());
            setup.redeem_and_verify(setup.request(&prefetched, first));
            let mut replay = setup.request(&prefetched, second);
            hot_signer()
                .inner_sign(&mut replay, &setup.descriptor)
                .expect("fresh peer nonces for changed input count");
            setup.assert_consumed(replay);
        }
    }
}
