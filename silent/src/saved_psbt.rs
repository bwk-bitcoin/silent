use std::{
    collections::BTreeSet,
    path::PathBuf,
    str::FromStr,
    time::{SystemTime, UNIX_EPOCH},
};

use bitcoin::{consensus::encode::deserialize_hex, Psbt, Transaction};
use bwk::bwk_electrum::tx_store::TxEntry;
use serde::{Deserialize, Serialize};

use crate::{
    ffi::{
        MusigSpendLeaf, SavedPsbtEntry, SavedPsbtInput, SavedPsbtMutation, SavedPsbtRecord,
        TransactionTemplate,
    },
    musig::decode_psbt,
    state_crypt,
};

const VERSION: u32 = 1;

#[derive(Serialize, Deserialize)]
struct SavedPsbtStateInternal {
    version: u32,
    records: Vec<SavedPsbtRecord>,
}

pub(crate) struct SavedPsbtStore {
    path: PathBuf,
    records: Vec<SavedPsbtRecord>,
    load_error: String,
    save_error: String,
}

impl SavedPsbtStore {
    pub(crate) fn load(path: PathBuf) -> Self {
        let mut store = Self {
            path,
            records: Vec::new(),
            load_error: String::new(),
            save_error: String::new(),
        };
        if let Err(error) = store.read() {
            log::error!("saved PSBT load: {error}");
            store.load_error = format!(
                "Saved PSBTs could not be loaded: {error}. The file will not be overwritten; new progress is only in memory and is not persisted"
            );
        }
        store
    }

    fn read(&mut self) -> Result<(), String> {
        self.path
            .try_exists()
            .map_err(|e| format!("{}: {e}", self.path.display()))?;
        let Some(text) = state_crypt::read_state(&self.path)? else {
            return Ok(());
        };
        let state: SavedPsbtStateInternal =
            serde_json::from_str(&text).map_err(|e| e.to_string())?;
        if state.version != VERSION {
            return Err(format!("unsupported saved PSBT version {}", state.version));
        }
        let mut ids = BTreeSet::new();
        for record in &state.records {
            if record.id.is_empty() || !ids.insert(record.id.clone()) {
                return Err("empty or duplicate saved PSBT id".into());
            }
            validate_record(record)?;
        }
        self.records = state.records;
        Ok(())
    }

    pub(crate) fn error(&self) -> String {
        if self.load_error.is_empty() {
            self.save_error.clone()
        } else {
            self.load_error.clone()
        }
    }

    pub(crate) fn load_error(&self) -> String {
        self.load_error.clone()
    }

    pub(crate) fn persist(&mut self) -> String {
        if !self.load_error.is_empty() {
            return self.load_error.clone();
        }
        let state = SavedPsbtStateInternal {
            version: VERSION,
            records: self.records.clone(),
        };
        let result = serde_json::to_string(&state)
            .map_err(|e| e.to_string())
            .and_then(|text| state_crypt::write_state(&self.path, &text));
        self.save_error = match result {
            Ok(()) => String::new(),
            Err(error) => {
                log::error!("saved PSBT write: {error}");
                format!(
                    "Latest transaction progress is only in memory and is not persisted: {error}"
                )
            }
        };
        self.save_error.clone()
    }

    pub(crate) fn create(
        &mut self,
        template: TransactionTemplate,
        spend: MusigSpendLeaf,
        psbt_v2_hex: String,
    ) -> Result<SavedPsbtMutation, String> {
        let mut random = [0; 16];
        getrandom::getrandom(&mut random)
            .map_err(|e| format!("Cannot create PSBT record id: {e}"))?;
        let id = hex::encode(random);
        if self.records.iter().any(|record| record.id == id) {
            return Err("duplicate generated saved PSBT id".into());
        }
        let now = timestamp()?;
        let record = SavedPsbtRecord {
            id: id.clone(),
            tx_template: template,
            spend,
            original_psbt_v2_hex: psbt_v2_hex.clone(),
            current_psbt_v2_hex: psbt_v2_hex,
            finalized_tx_hex: String::new(),
            broadcast: false,
            label: String::new(),
            created_at: now,
            updated_at: now,
        };
        validate_record(&record)?;
        self.records.push(record);
        Ok(self.saved(id))
    }

    fn saved(&mut self, record_id: String) -> SavedPsbtMutation {
        SavedPsbtMutation {
            is_ok: true,
            record_id,
            save_error: self.persist(),
            error: String::new(),
        }
    }

    pub(crate) fn get(&self, id: &str) -> Result<&SavedPsbtRecord, String> {
        self.records
            .iter()
            .find(|r| r.id == id)
            .ok_or_else(|| format!("Saved PSBT '{id}' not found"))
    }

    fn get_mut(&mut self, id: &str) -> Result<&mut SavedPsbtRecord, String> {
        self.records
            .iter_mut()
            .find(|r| r.id == id)
            .ok_or_else(|| format!("Saved PSBT '{id}' not found"))
    }

    pub(crate) fn records(&self) -> &[SavedPsbtRecord] {
        &self.records
    }

    pub(crate) fn update(&mut self, id: &str, psbt: &str) -> Result<SavedPsbtMutation, String> {
        let mut next = self.get(id)?.clone();
        next.current_psbt_v2_hex = psbt.to_string();
        validate_record(&next)?;
        next.updated_at = timestamp()?;
        *self.get_mut(id)? = next;
        Ok(self.saved(id.to_string()))
    }

    pub(crate) fn label(&mut self, id: &str, label: String) -> Result<SavedPsbtMutation, String> {
        let now = timestamp()?;
        let record = self.get_mut(id)?;
        record.label = label;
        record.updated_at = now;
        Ok(self.saved(id.to_string()))
    }

    pub(crate) fn delete(&mut self, id: &str) -> Result<SavedPsbtMutation, String> {
        self.get(id)?;
        self.records.retain(|r| r.id != id);
        Ok(self.saved(id.to_string()))
    }

    pub(crate) fn finalized(&mut self, previous: &str, psbt: &str, tx: &str) -> Result<(), String> {
        let ids: Vec<_> = self
            .records
            .iter()
            .filter(|r| r.current_psbt_v2_hex == previous)
            .map(|r| r.id.clone())
            .collect();
        let now = timestamp()?;
        for id in ids {
            let mut next = self.get(&id)?.clone();
            next.current_psbt_v2_hex = psbt.to_string();
            next.finalized_tx_hex = tx.to_string();
            next.updated_at = now;
            validate_record(&next)?;
            *self.get_mut(&id)? = next;
        }
        self.persist();
        Ok(())
    }

    pub(crate) fn broadcast(&mut self, tx: &Transaction) -> Result<(), String> {
        let now = timestamp()?;
        let txid = tx.compute_txid();
        for record in &mut self.records {
            if record.finalized_tx_hex.is_empty() {
                continue;
            }
            let finalized: Transaction =
                deserialize_hex(&record.finalized_tx_hex).map_err(|e| e.to_string())?;
            if finalized.compute_txid() == txid {
                record.broadcast = true;
                record.updated_at = now;
            }
        }
        self.persist();
        Ok(())
    }
}

fn timestamp() -> Result<u64, String> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|t| t.as_secs())
        .map_err(|e| e.to_string())
}

fn validate_record(record: &SavedPsbtRecord) -> Result<(), String> {
    let original = decode_psbt(&record.original_psbt_v2_hex)?;
    let current = decode_psbt(&record.current_psbt_v2_hex)?;
    if current.unsigned_tx != original.unsigned_tx {
        return Err("Updated PSBT names a different transaction".into());
    }
    if !record.finalized_tx_hex.is_empty() {
        let tx: Transaction = deserialize_hex(&record.finalized_tx_hex)
            .map_err(|e| format!("Invalid finalized transaction: {e}"))?;
        let extracted = current.clone().extract_tx_unchecked_fee_rate();
        if extracted != tx {
            return Err("Finalized transaction does not match the saved PSBT".into());
        }
    }
    frozen_entry(record)?;
    Ok(())
}

pub(crate) fn frozen_entry(record: &SavedPsbtRecord) -> Result<SavedPsbtEntry, String> {
    let psbt = decode_psbt(&record.original_psbt_v2_hex)?;
    let funding = psbt
        .iter_funding_utxos()
        .collect::<Result<Vec<_>, _>>()
        .map_err(|e| format!("Cannot restore input values: {e}"))?;
    let inputs: Vec<_> = psbt
        .unsigned_tx
        .input
        .iter()
        .zip(funding)
        .map(|(input, utxo)| SavedPsbtInput {
            outpoint: input.previous_output.to_string(),
            value: utxo.value.to_sat(),
        })
        .collect();
    let mut template = record.tx_template.clone();
    template.input_outpoints = inputs.iter().map(|input| input.outpoint.clone()).collect();
    template.fee = psbt
        .fee()
        .map_err(|e| format!("Cannot restore fee: {e}"))?
        .to_sat();
    resolve_outputs(&mut template, &psbt)?;
    Ok(SavedPsbtEntry {
        record: record.clone(),
        txid: psbt.unsigned_tx.compute_txid().to_string(),
        frozen_template: template,
        input_total: inputs.iter().map(|input| input.value).sum(),
        output_total: psbt
            .unsigned_tx
            .output
            .iter()
            .map(|output| output.value.to_sat())
            .sum(),
        inputs,
        ..SavedPsbtEntry::default()
    })
}

fn resolve_outputs(template: &mut TransactionTemplate, psbt: &Psbt) -> Result<(), String> {
    let mut used = BTreeSet::new();
    // Fixed recipients are matched first because the builder puts MAX last.
    let mut order: Vec<_> = (0..template.outputs.len()).collect();
    order.sort_by_key(|index| template.outputs[*index].max);
    for index in order {
        let output = &mut template.outputs[index];
        let script = bitcoin::Address::from_str(&output.address)
            .map_err(|e| e.to_string())?
            .assume_checked()
            .script_pubkey();
        let (vout, resolved) = psbt
            .unsigned_tx
            .output
            .iter()
            .enumerate()
            .find(|(vout, txout)| {
                !used.contains(vout)
                    && txout.script_pubkey == script
                    && (output.max || txout.value.to_sat() == output.amount)
            })
            .ok_or_else(|| format!("Saved recipient '{}' not found in PSBT", output.address))?;
        used.insert(vout);
        output.amount = resolved.value.to_sat();
        output.max = false;
    }
    Ok(())
}

pub(crate) fn apply_chain_state(entry: &mut SavedPsbtEntry, history: &[TxEntry]) {
    entry.confirmed_height = 0;
    entry.in_history = false;
    let mut conflicts = BTreeSet::new();
    for tx in history {
        if tx.txid().to_string() == entry.txid {
            entry.in_history = true;
            entry.confirmed_height = tx.height().unwrap_or(0) as u32;
        } else if tx.tx().input.iter().any(|input| {
            entry
                .inputs
                .iter()
                .any(|saved| saved.outpoint == input.previous_output.to_string())
        }) {
            conflicts.insert(tx.txid().to_string());
        }
    }
    entry.conflicting_txids = conflicts.into_iter().collect();
    entry.warning = if entry.conflicting_txids.is_empty() {
        String::new()
    } else {
        "Inputs are spent by another transaction. Signing and broadcast can still be attempted"
            .into()
    };
}

pub(crate) fn mutation_error(error: String) -> SavedPsbtMutation {
    SavedPsbtMutation {
        error,
        ..SavedPsbtMutation::default()
    }
}

#[cfg(test)]
mod tests {
    use std::fs;

    use bitcoin::{
        absolute::LockTime, consensus::encode::serialize_hex, hashes::Hash, psbt::raw::Key,
        transaction::Version, Address, Amount, BlockHash, Network, OutPoint, Psbt, ScriptBuf,
        Transaction, TxIn, TxOut, Txid, WScriptHash, Witness,
    };
    use bwk::{
        bwk_electrum::tx_store::{Inclusion, TxEntry},
        bwk_tx::bwk_psbt::PsbtV2,
    };
    use temp_dir::TempDir;

    use crate::{
        ffi::{MusigSpendLeaf, Output, TransactionTemplate},
        musig::decode_psbt,
        saved_psbt::{apply_chain_state, frozen_entry, SavedPsbtStore},
        state_crypt,
    };

    fn fixture() -> (TransactionTemplate, MusigSpendLeaf, Psbt) {
        state_crypt::unlock_for_tests();
        let script = ScriptBuf::new_p2wsh(&WScriptHash::from_byte_array([3; 32]));
        let address = Address::from_script(&script, Network::Regtest)
            .expect("recipient address")
            .to_string();
        let tx = Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: vec![TxIn {
                previous_output: OutPoint::new(Txid::from_byte_array([7; 32]), 2),
                ..TxIn::default()
            }],
            output: vec![
                TxOut {
                    value: Amount::from_sat(40_000),
                    script_pubkey: script.clone(),
                },
                TxOut {
                    value: Amount::from_sat(79_000),
                    script_pubkey: script.clone(),
                },
            ],
        };
        let mut psbt = Psbt::from_unsigned_tx(tx).expect("unsigned transaction");
        psbt.inputs[0].witness_utxo = Some(TxOut {
            value: Amount::from_sat(120_000),
            script_pubkey: script,
        });
        let template = TransactionTemplate {
            outputs: vec![
                Output {
                    address: address.clone(),
                    amount: 40_000,
                    label: "fixed".into(),
                    max: false,
                },
                Output {
                    address,
                    amount: 0,
                    label: "MAX".into(),
                    max: true,
                },
            ],
            fee_rate: 2.0,
            fee: 0,
            input_outpoints: Vec::new(),
        };
        let spend = MusigSpendLeaf {
            key_path: true,
            fingerprints: vec!["deadbeef".into()],
            ..MusigSpendLeaf::default()
        };
        (template, spend, psbt)
    }

    fn v2(psbt: &Psbt) -> String {
        hex::encode(
            PsbtV2::from_bitcoin_psbt(psbt.clone())
                .expect("PSBT v2")
                .serialize()
                .expect("serialize PSBT"),
        )
    }

    #[test]
    fn exact_psbts_and_frozen_max_inputs_and_fee_survive_reload() {
        let tmp = TempDir::new().expect("temporary state directory");
        let path = tmp.path().join("saved_psbts.json");
        let (template, spend, mut psbt) = fixture();
        let original = v2(&psbt);
        let mut store = SavedPsbtStore::load(path.clone());
        let saved = store
            .create(template, spend, original.clone())
            .expect("create saved PSBT");
        assert!(
            saved.save_error.is_empty(),
            "persist prepared PSBT: {}",
            saved.save_error
        );
        psbt.inputs[0].unknown.insert(
            Key {
                type_value: 0xfa,
                key: vec![1],
            },
            vec![5, 6, 7],
        );
        let current = v2(&psbt).to_uppercase();
        let updated = store
            .update(&saved.record_id, &current)
            .expect("persist progress");
        assert!(
            updated.save_error.is_empty(),
            "persist signing progress: {}",
            updated.save_error
        );
        drop(store);
        let reloaded = SavedPsbtStore::load(path.clone());
        assert!(
            reloaded.error().is_empty(),
            "reload encrypted records: {}",
            reloaded.error()
        );
        let record = reloaded.get(&saved.record_id).expect("same record id");
        assert_eq!(
            record.original_psbt_v2_hex, original,
            "original bytes remain authoritative"
        );
        assert_eq!(
            record.current_psbt_v2_hex, current,
            "current bytes are not normalized or reserialized"
        );
        assert_eq!(
            record.spend.fingerprints,
            vec!["deadbeef"],
            "selected signers survive"
        );
        assert!(
            record.tx_template.outputs[1].max,
            "original MAX template survives"
        );
        let entry = frozen_entry(record).expect("restore exact transaction");
        assert_eq!(
            entry.frozen_template.outputs[1].amount, 79_000,
            "MAX is resolved from the PSBT"
        );
        assert!(
            !entry.frozen_template.outputs[1].max,
            "restored MAX cannot be recalculated"
        );
        assert_eq!(
            entry.frozen_template.fee, 1_000,
            "actual absolute fee is frozen"
        );
        assert_eq!(
            entry.inputs[0].value, 120_000,
            "input amount comes from funding UTXO"
        );
        assert_eq!(
            entry.frozen_template.input_outpoints,
            vec![psbt.unsigned_tx.input[0].previous_output.to_string()],
            "automatic input selection is frozen"
        );
        assert!(
            state_crypt::is_sealed(&fs::read(path).expect("saved file")),
            "records use encrypted state storage"
        );
    }

    #[test]
    fn overlapping_records_have_independent_ids_and_explicit_deletion() {
        let tmp = TempDir::new().expect("temporary state directory");
        let path = tmp.path().join("saved_psbts.json");
        let (template, spend, psbt) = fixture();
        let mut store = SavedPsbtStore::load(path.clone());
        let first = store
            .create(template.clone(), spend.clone(), v2(&psbt))
            .expect("first record");
        let second = store
            .create(template, spend, v2(&psbt))
            .expect("overlapping record");
        assert_ne!(first.record_id, second.record_id, "txid is not a record id");
        store
            .label(&first.record_id, "first attempt".into())
            .expect("edit label");
        assert!(
            store
                .get(&second.record_id)
                .expect("second record")
                .label
                .is_empty(),
            "metadata edits are independent"
        );
        store.delete(&first.record_id).expect("explicit deletion");
        assert!(
            store.get(&first.record_id).is_err(),
            "deleted record is gone from memory"
        );
        assert!(
            store.delete("missing").is_err(),
            "unknown deletions are refused"
        );
        let reloaded = SavedPsbtStore::load(path);
        assert_eq!(
            reloaded.records().len(),
            1,
            "only explicit deletion removes a record"
        );
        assert_eq!(
            reloaded.records()[0].id,
            second.record_id,
            "overlapping record survives"
        );
    }

    #[test]
    fn invalid_updates_do_not_replace_current_progress() {
        let tmp = TempDir::new().expect("temporary state directory");
        let (template, spend, mut psbt) = fixture();
        let original = v2(&psbt);
        let mut store = SavedPsbtStore::load(tmp.path().join("saved_psbts.json"));
        let saved = store
            .create(template, spend, original.clone())
            .expect("saved record");
        psbt.unsigned_tx.output[0].value = Amount::from_sat(39_000);
        assert!(
            store
                .update(&saved.record_id, &v2(&psbt))
                .expect_err("changed transaction must fail")
                .contains("different transaction"),
            "updates cannot change recipients, inputs or fee"
        );
        assert!(
            store.update(&saved.record_id, "broken").is_err(),
            "invalid PSBT must fail"
        );
        assert_eq!(
            store
                .get(&saved.record_id)
                .expect("saved record")
                .current_psbt_v2_hex,
            original,
            "failed updates keep latest valid progress"
        );
    }

    #[test]
    fn failed_save_keeps_memory_and_retry_persists_it() {
        let tmp = TempDir::new().expect("temporary state directory");
        let path = tmp.path().join("saved_psbts.json");
        let (template, spend, mut psbt) = fixture();
        let mut store = SavedPsbtStore::load(path.clone());
        let saved = store
            .create(template, spend, v2(&psbt))
            .expect("saved record");
        fs::create_dir(path.with_extension("tmp")).expect("block atomic write");
        psbt.inputs[0].unknown.insert(
            Key {
                type_value: 0xfa,
                key: vec![1],
            },
            vec![8],
        );
        let current = v2(&psbt);
        let updated = store
            .update(&saved.record_id, &current)
            .expect("signing progress remains usable");
        assert!(updated.is_ok, "save failure is not a signing failure");
        assert!(
            updated.save_error.contains("not persisted"),
            "save failure must be visible: {}",
            updated.save_error
        );
        assert_eq!(
            store
                .get(&saved.record_id)
                .expect("in-memory record")
                .current_psbt_v2_hex,
            current,
            "latest progress remains in memory"
        );
        let old = SavedPsbtStore::load(path.clone());
        assert_ne!(
            old.get(&saved.record_id)
                .expect("old persisted record")
                .current_psbt_v2_hex,
            current,
            "failed save leaves disk unchanged"
        );
        fs::remove_dir(path.with_extension("tmp")).expect("unblock write");
        assert!(
            store.persist().is_empty(),
            "retry should save latest progress"
        );
        assert_eq!(
            SavedPsbtStore::load(path)
                .get(&saved.record_id)
                .expect("reloaded latest progress")
                .current_psbt_v2_hex,
            current,
            "retry saves memory, not stale disk state"
        );
    }

    #[test]
    fn corrupt_state_is_not_overwritten_even_when_new_progress_is_saved() {
        let tmp = TempDir::new().expect("temporary state directory");
        let path = tmp.path().join("saved_psbts.json");
        let (template, spend, psbt) = fixture();
        state_crypt::write_state(&path, "{broken").expect("write encrypted corrupt state");
        let original = fs::read(&path).expect("corrupt ciphertext");
        let mut store = SavedPsbtStore::load(path.clone());
        assert!(
            !store.load_error().is_empty(),
            "parse failure must be visible"
        );
        let saved = store
            .create(template, spend, v2(&psbt))
            .expect("new in-memory progress");
        assert!(
            !saved.save_error.is_empty(),
            "new record warns that it cannot persist"
        );
        assert!(
            store.get(&saved.record_id).is_ok(),
            "signing can continue with memory state"
        );
        assert!(
            !store.persist().is_empty(),
            "retry cannot overwrite an unreadable file"
        );
        assert_eq!(
            fs::read(path).expect("preserved file"),
            original,
            "failed load fences all disk writes"
        );
    }

    #[test]
    fn finalized_and_broadcast_records_survive_restart_until_deleted() {
        let tmp = TempDir::new().expect("temporary state directory");
        let path = tmp.path().join("saved_psbts.json");
        let (template, spend, mut psbt) = fixture();
        let original = v2(&psbt);
        let mut store = SavedPsbtStore::load(path.clone());
        let saved = store
            .create(template, spend, original.clone())
            .expect("saved record");
        psbt.inputs[0].final_script_witness = Some(Witness::from_slice(&[vec![1, 2, 3]]));
        let finalized = v2(&psbt);
        let tx = psbt.extract_tx_unchecked_fee_rate();
        store
            .finalized(&original, &finalized, &serialize_hex(&tx))
            .expect("persist finalization");
        let awaiting = SavedPsbtStore::load(path.clone());
        let record = awaiting
            .get(&saved.record_id)
            .expect("finalized record after restart");
        assert_eq!(
            record.current_psbt_v2_hex, finalized,
            "finalized PSBT survives restart"
        );
        assert_eq!(
            record.finalized_tx_hex,
            serialize_hex(&tx),
            "transaction survives restart before broadcast"
        );
        assert!(!record.broadcast, "finalization is not broadcast");
        store.broadcast(&tx).expect("persist broadcast");
        let broadcast = SavedPsbtStore::load(path);
        assert!(
            broadcast
                .get(&saved.record_id)
                .expect("broadcast record retained")
                .broadcast,
            "broadcast flag survives reload"
        );
        assert_eq!(
            broadcast.records().len(),
            1,
            "broadcast does not delete the record"
        );
        assert_eq!(
            decode_psbt(&finalized)
                .expect("finalized PSBT")
                .extract_tx_unchecked_fee_rate(),
            tx,
            "saved PSBT extracts exact finalized transaction"
        );
    }

    #[test]
    fn chain_state_tracks_confirmation_reorg_and_conflicting_spend() {
        let tmp = TempDir::new().expect("temporary state directory");
        let (template, spend, psbt) = fixture();
        let mut store = SavedPsbtStore::load(tmp.path().join("saved_psbts.json"));
        let saved = store
            .create(template, spend, v2(&psbt))
            .expect("saved record");
        let mut entry = frozen_entry(store.get(&saved.record_id).expect("saved record"))
            .expect("restored entry");
        let unconfirmed = TxEntry::unconfirmed(psbt.unsigned_tx.clone());
        let mut json = serde_json::to_value(&unconfirmed).expect("transaction entry");
        json["inclusion"] = serde_json::to_value(Inclusion::Verified {
            height: 42,
            block_hash: BlockHash::from_byte_array([4; 32]),
        })
        .expect("confirmed inclusion");
        let confirmed: TxEntry = serde_json::from_value(json).expect("confirmed entry");
        apply_chain_state(&mut entry, &[confirmed]);
        assert_eq!(
            entry.confirmed_height, 42,
            "confirmed entry can be hidden from default list"
        );
        assert!(entry.warning.is_empty(), "own spend is not a conflict");
        apply_chain_state(&mut entry, &[unconfirmed]);
        assert_eq!(
            entry.confirmed_height, 0,
            "reorg makes record visible again"
        );
        assert!(entry.in_history, "unconfirmed transaction remains known");
        apply_chain_state(&mut entry, &[]);
        assert!(
            !entry.in_history,
            "removed chain entry is not cached forever"
        );
        let mut conflict = psbt.unsigned_tx;
        conflict.output[0].value = Amount::from_sat(38_000);
        let conflict_id = conflict.compute_txid().to_string();
        apply_chain_state(&mut entry, &[TxEntry::unconfirmed(conflict)]);
        assert_eq!(
            entry.conflicting_txids,
            vec![conflict_id],
            "overlapping chain spend is identified"
        );
        assert!(
            !entry.warning.is_empty(),
            "conflict is a visible warning, not a signing gate"
        );
        apply_chain_state(&mut entry, &[]);
        assert!(
            entry.conflicting_txids.is_empty() && entry.warning.is_empty(),
            "removed conflicts are cleared"
        );
    }
}
