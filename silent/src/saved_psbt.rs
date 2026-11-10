use std::{
    collections::BTreeSet,
    str::FromStr,
    sync::Arc,
    time::{SystemTime, UNIX_EPOCH},
};

use bitcoin::{consensus::encode::deserialize_hex, Psbt, Transaction, Txid};
use bwk::bwk_electrum::tx_store::TxEntry;
use serde::{Deserialize, Serialize};

use crate::{
    account_storage::{AccountStorage, Factory},
    ffi::{
        MusigSpendLeaf, SavedPsbtAttempt, SavedPsbtEntry, SavedPsbtInput, SavedPsbtMutation,
        SavedPsbtRecord, TransactionTemplate,
    },
    keys::musig_sessions::attempt_id,
    musig::decode_psbt,
};

#[cfg(test)]
use crate::state_crypt;
#[cfg(test)]
use std::path::PathBuf;

const VERSION: u32 = 1;

#[derive(Serialize, Deserialize)]
struct SavedPsbtStateInternal {
    version: u32,
    records: Vec<SavedPsbtRecord>,
}

pub(crate) struct SavedPsbtStore {
    location: SavedPsbtLocation,
    records: Vec<SavedPsbtRecord>,
    load_error: String,
    save_error: String,
}

enum SavedPsbtLocation {
    Logical {
        storage: AccountStorage,
        owner: Arc<Factory>,
    },
    #[cfg(test)]
    File(PathBuf),
}

impl SavedPsbtStore {
    #[cfg(test)]
    pub(crate) fn load(path: PathBuf) -> Self {
        Self::load_location(SavedPsbtLocation::File(path))
    }

    pub(crate) fn load_logical(storage: AccountStorage, owner: Arc<Factory>) -> Self {
        Self::load_location(SavedPsbtLocation::Logical { storage, owner })
    }

    pub(crate) fn bind_factory(&mut self, owner: Arc<Factory>) {
        match &mut self.location {
            SavedPsbtLocation::Logical { owner: current, .. } => *current = owner,
            #[cfg(test)]
            SavedPsbtLocation::File(_) => (),
        }
    }

    fn load_location(location: SavedPsbtLocation) -> Self {
        let mut store = Self {
            location,
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
        let text = match &self.location {
            SavedPsbtLocation::Logical { storage, .. } => storage
                .read("saved_psbts.json")?
                .map(String::from_utf8)
                .transpose()
                .map_err(|e| e.to_string())?,
            #[cfg(test)]
            SavedPsbtLocation::File(path) => {
                path.try_exists().map_err(|e| e.to_string())?;
                state_crypt::read_state(path)?
            }
        };
        let Some(text) = text else {
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
            .and_then(|text| match &self.location {
                SavedPsbtLocation::Logical { storage, owner } => {
                    let _lease = if owner.is_closing() {
                        Some(storage.lease()?)
                    } else {
                        None
                    };
                    storage.write("saved_psbts.json", text.into_bytes(), false)
                }
                #[cfg(test)]
                SavedPsbtLocation::File(path) => {
                    if self
                        .records
                        .iter()
                        .any(|record| !record.archived_attempts.is_empty())
                    {
                        state_crypt::write_state_durable(path, &text)
                    } else {
                        state_crypt::write_state(path, &text)
                    }
                }
            });
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
            current_psbt_v2_hex: psbt_v2_hex.clone(),
            attempt_id: 0,
            baseline_psbt_v2_hex: psbt_v2_hex,
            archived_attempts: Vec::new(),
            reset_pending: false,
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
        if next.reset_pending {
            return Err("Signing attempt is being reset; retry Reset before signing".into());
        }
        next.current_psbt_v2_hex = psbt.to_string();
        validate_record(&next)?;
        next.updated_at = timestamp()?;
        *self.get_mut(id)? = next;
        Ok(self.saved(id.to_string()))
    }

    pub(crate) fn reset(
        &mut self,
        id: &str,
        retire: impl FnOnce(&str, &str) -> Result<String, String>,
    ) -> Result<String, String> {
        let previous = self.get(id)?.clone();
        if !previous.finalized_tx_hex.is_empty() {
            return Err("Cannot reset a finalized transaction".into());
        }
        let next_id = previous
            .attempt_id
            .checked_add(1)
            .ok_or("Attempt id overflow")?;
        self.archive(id)?;
        // Retirement may partly succeed: keep the old PSBT fenced until a retry completes.
        let psbt = retire(
            &previous.current_psbt_v2_hex,
            &previous.original_psbt_v2_hex,
        )?;
        let marker = attempt_id(&decode_psbt(&psbt)?.proprietary)?;
        if marker.is_none()
            || marker == attempt_id(&decode_psbt(&previous.current_psbt_v2_hex)?.proprietary)?
        {
            return Err("Reset did not create a fresh MuSig signing attempt ID".into());
        }
        let mut next = self.get(id)?.clone();
        next.attempt_id = next_id;
        next.baseline_psbt_v2_hex = psbt.clone();
        next.current_psbt_v2_hex = psbt.clone();
        next.reset_pending = false;
        next.updated_at = timestamp()?;
        validate_record(&next)?;
        *self.get_mut(id)? = next;
        self.persist();
        Ok(psbt)
    }

    fn archive(&mut self, id: &str) -> Result<(), String> {
        let previous = self.get(id)?.clone();
        let mut next = previous.clone();
        if !next.reset_pending {
            next.archived_attempts.push(SavedPsbtAttempt {
                attempt_id: next.attempt_id,
                baseline_psbt_v2_hex: if next.baseline_psbt_v2_hex.is_empty() {
                    next.original_psbt_v2_hex.clone()
                } else {
                    next.baseline_psbt_v2_hex.clone()
                },
                current_psbt_v2_hex: next.current_psbt_v2_hex.clone(),
            });
            next.reset_pending = true;
        }
        validate_record(&next)?;
        *self.get_mut(id)? = next;
        let result = self.persist_archive();
        if result.is_err() {
            *self.get_mut(id)? = previous;
        }
        result
    }

    fn persist_archive(&mut self) -> Result<(), String> {
        let error = self.persist();
        if !error.is_empty() {
            return Err(format!(
                "Reset stopped: cannot archive the signing attempt. {error}. Retry Reset"
            ));
        }
        Ok(())
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

    /// Marks the saved transaction of `txid` broadcast.
    pub(crate) fn broadcast(&mut self, txid: Txid) -> Result<(), String> {
        let now = timestamp()?;
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
    let baseline = if record.baseline_psbt_v2_hex.is_empty() {
        original.clone()
    } else {
        decode_psbt(&record.baseline_psbt_v2_hex)?
    };
    if baseline.unsigned_tx != original.unsigned_tx {
        return Err("Attempt baseline names a different transaction".into());
    }
    let active_marker = validate_attempt_marker(&baseline, &current, record.attempt_id)?;
    if record.attempt_id == 0 && active_marker != attempt_id(&original.proprietary)? {
        return Err("Initial attempt marker differs from the original baseline".into());
    }
    let mut ids = BTreeSet::new();
    let mut markers = BTreeSet::new();
    for attempt in &record.archived_attempts {
        if !ids.insert(attempt.attempt_id)
            || attempt.attempt_id > record.attempt_id
            || (attempt.attempt_id == record.attempt_id && !record.reset_pending)
        {
            return Err("Invalid archived signing attempt identity".into());
        }
        let archived_baseline = decode_psbt(&attempt.baseline_psbt_v2_hex)?;
        let archived_current = decode_psbt(&attempt.current_psbt_v2_hex)?;
        if archived_baseline.unsigned_tx != original.unsigned_tx
            || archived_current.unsigned_tx != original.unsigned_tx
        {
            return Err("Archived PSBT names a different transaction".into());
        }
        let marker =
            validate_attempt_marker(&archived_baseline, &archived_current, attempt.attempt_id)?;
        if attempt.attempt_id == 0 && marker != attempt_id(&original.proprietary)? {
            return Err(
                "Archived initial attempt marker differs from the original baseline".into(),
            );
        }
        if marker.is_some_and(|marker| !markers.insert(marker)) {
            return Err("Archived signing attempts reuse a MuSig attempt ID".into());
        }
    }
    if !record.reset_pending && active_marker.is_some_and(|marker| markers.contains(&marker)) {
        return Err("Active signing attempt reuses an archived MuSig attempt ID".into());
    }
    if record.reset_pending
        && !record.archived_attempts.iter().any(|attempt| {
            attempt.attempt_id == record.attempt_id
                && attempt.current_psbt_v2_hex == record.current_psbt_v2_hex
        })
    {
        return Err("Pending reset has no exact archived snapshot".into());
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

fn validate_attempt_marker(
    baseline: &Psbt,
    current: &Psbt,
    id: u64,
) -> Result<Option<[u8; 32]>, String> {
    let marker = attempt_id(&baseline.proprietary)?;
    if marker != attempt_id(&current.proprietary)? || (id != 0 && marker.is_none()) {
        return Err("Current PSBT MuSig attempt ID does not match its baseline".into());
    }
    Ok(marker)
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
        absolute::LockTime,
        consensus::encode::serialize_hex,
        hashes::Hash,
        psbt::raw::{Key, ProprietaryKey},
        secp256k1::{PublicKey, Secp256k1, SecretKey},
        transaction::Version,
        Address, Amount, BlockHash, Network, OutPoint, Psbt, ScriptBuf, Transaction, TxIn, TxOut,
        Txid, WScriptHash, Witness,
    };
    use bwk::{
        bwk_electrum::tx_store::{Inclusion, TxEntry},
        bwk_tx::bwk_psbt::{
            musig::{
                set_musig_input_partial_sig, set_musig_input_pub_nonce, MusigPartialSig,
                MusigPubNonce,
            },
            PsbtV2,
        },
    };
    use temp_dir::TempDir;

    use crate::{
        ffi::{MusigSpendLeaf, Output, TransactionTemplate},
        musig::decode_psbt,
        saved_psbt::{apply_chain_state, frozen_entry, validate_record, SavedPsbtStore},
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

    fn signing_progress(psbt: &Psbt, marker: u8) -> String {
        let mut psbt = psbt.clone();
        let key = PublicKey::from_secret_key(
            &Secp256k1::new(),
            &SecretKey::from_slice(&[marker; 32]).expect("test key"),
        );
        let mut nonce = [0; 66];
        nonce[..33].copy_from_slice(&key.serialize());
        nonce[33..].copy_from_slice(&key.serialize());
        set_musig_input_pub_nonce(
            &mut psbt.inputs[0],
            &MusigPubNonce {
                participant: key,
                aggregate: key,
                leaf_hash: None,
                nonce,
            },
        );
        set_musig_input_partial_sig(
            &mut psbt.inputs[0],
            &MusigPartialSig {
                participant: key,
                aggregate: key,
                leaf_hash: None,
                signature: [marker; 32],
            },
        );
        psbt.inputs[0].unknown.insert(
            Key {
                type_value: 0xfa,
                key: vec![marker],
            },
            vec![marker; 7],
        );
        v2(&psbt).to_uppercase()
    }

    fn next_attempt(psbt: &Psbt, marker: u8) -> String {
        let mut psbt = psbt.clone();
        psbt.proprietary.insert(
            ProprietaryKey {
                prefix: b"SILENT_MUSIG_ATTEMPT".to_vec(),
                subtype: 0,
                key: Vec::new(),
            },
            vec![marker; 32],
        );
        v2(&psbt)
    }

    #[test]
    fn every_archive_carrying_replacement_syncs_before_rename() {
        for stage in [
            state_crypt::DurableWriteStage::TempWritten,
            state_crypt::DurableWriteStage::TempSynced,
            state_crypt::DurableWriteStage::Renamed,
        ] {
            let tmp = TempDir::new().expect("temporary state directory");
            let path = tmp.path().join("saved_psbts.json");
            let (template, spend, psbt) = fixture();
            let mut store = SavedPsbtStore::load(path.clone());
            let id = store
                .create(template, spend, v2(&psbt))
                .expect("saved record")
                .record_id;
            let progress = signing_progress(&psbt, 1);
            store.update(&id, &progress).expect("progress before reset");
            store
                .reset(&id, |_, _| Ok(next_attempt(&psbt, 2)))
                .expect("durable archive and active attempt");
            let before = fs::read(&path).expect("durable active state");
            state_crypt::fail_durable_write_at(stage);
            let changed = store
                .label(&id, "new label".into())
                .expect("metadata remains usable in memory");
            assert!(
                !changed.save_error.is_empty(),
                "injected {stage:?} error must be visible"
            );
            let reloaded = SavedPsbtStore::load(path.clone());
            assert!(
                reloaded.error().is_empty(),
                "replacement remains complete at {stage:?}: {}",
                reloaded.error()
            );
            let record = reloaded.get(&id).expect("recovered archive");
            assert_eq!(
                record.archived_attempts[0].current_psbt_v2_hex, progress,
                "every complete inode retains the exact retired attempt at {stage:?}"
            );
            if stage != state_crypt::DurableWriteStage::Renamed {
                assert_eq!(
                    fs::read(&path).expect("old inode"),
                    before,
                    "errors before rename never replace the last durable inode at {stage:?}"
                );
                assert!(
                    record.label.is_empty(),
                    "unpublished metadata stays off disk"
                );
            } else {
                assert_eq!(
                    record.label, "new label",
                    "replacement-phase error may leave the fully synced new file installed"
                );
            }
            assert!(
                store.persist().is_empty(),
                "retry syncs latest metadata at {stage:?}"
            );
        }
    }

    #[test]
    fn interrupted_active_publication_keeps_the_retired_attempt_and_recovery_fence() {
        for stage in [
            state_crypt::DurableWriteStage::TempWritten,
            state_crypt::DurableWriteStage::TempSynced,
            state_crypt::DurableWriteStage::Renamed,
        ] {
            let tmp = TempDir::new().expect("temporary state directory");
            let path = tmp.path().join("saved_psbts.json");
            let (template, spend, psbt) = fixture();
            let mut store = SavedPsbtStore::load(path.clone());
            let id = store
                .create(template, spend, v2(&psbt))
                .expect("saved record")
                .record_id;
            let progress = signing_progress(&psbt, 1);
            store.update(&id, &progress).expect("pre-reset progress");
            let next = next_attempt(&psbt, 2);
            store
                .reset(&id, |_, _| {
                    let disk = SavedPsbtStore::load(path.clone());
                    let record = disk.get(&id).expect("archive before retiring nonces");
                    assert!(
                        record.reset_pending,
                        "old active attempt is fenced before retirement"
                    );
                    assert_eq!(
                        record.archived_attempts[0].current_psbt_v2_hex, progress,
                        "retirement sees a complete durable archive"
                    );
                    state_crypt::fail_durable_write_at(stage);
                    Ok(next.clone())
                })
                .expect("new attempt remains usable in memory after a publication error");
            assert!(
                !store.error().is_empty(),
                "publication error is visible at {stage:?}"
            );
            drop(store);
            let reloaded = SavedPsbtStore::load(path);
            let record = reloaded.get(&id).expect("recovered reset");
            assert_eq!(
                record.archived_attempts[0].current_psbt_v2_hex, progress,
                "retired nonce/partial progress survives either complete file at {stage:?}"
            );
            if stage != state_crypt::DurableWriteStage::Renamed {
                assert_eq!(
                    record.current_psbt_v2_hex, progress,
                    "interruption before active replacement retains the pre-reset current bytes"
                );
                assert!(
                    record.reset_pending,
                    "a retired old attempt cannot sign on reload"
                );
            } else {
                assert_eq!(
                    record.current_psbt_v2_hex, next,
                    "published new active file was synced before rename"
                );
                assert!(
                    !record.reset_pending,
                    "the installed new attempt is not the retired one"
                );
            }
        }
    }

    #[test]
    fn archive_phase_errors_never_invoke_nonce_retirement() {
        for stage in [
            state_crypt::DurableWriteStage::TempWritten,
            state_crypt::DurableWriteStage::TempSynced,
            state_crypt::DurableWriteStage::Renamed,
        ] {
            let tmp = TempDir::new().expect("temporary state directory");
            let path = tmp.path().join("saved_psbts.json");
            let (template, spend, psbt) = fixture();
            let mut store = SavedPsbtStore::load(path.clone());
            let id = store
                .create(template, spend, v2(&psbt))
                .expect("saved record")
                .record_id;
            let progress = signing_progress(&psbt, 1);
            store.update(&id, &progress).expect("pre-reset progress");
            let before = fs::read(&path).expect("old active file");
            state_crypt::fail_durable_write_at(stage);
            let error = store
                .reset(&id, |_, _| {
                    panic!("archive fault must precede nonce retirement")
                })
                .expect_err("archive checkpoint failure");
            assert!(
                error.contains("cannot archive"),
                "archive fault is actionable: {error}"
            );
            assert_eq!(
                store
                    .get(&id)
                    .expect("in-memory current")
                    .current_psbt_v2_hex,
                progress,
                "failed archive keeps exact current state"
            );
            if stage != state_crypt::DurableWriteStage::Renamed {
                assert_eq!(
                    fs::read(&path).expect("old durable file"),
                    before,
                    "failed temp sync or rename preserves old file"
                );
            }
            let mut reloaded = SavedPsbtStore::load(path);
            reloaded
                .reset(&id, |_, _| Ok(next_attempt(&psbt, 2)))
                .expect("retry after reload");
            assert_eq!(
                reloaded
                    .get(&id)
                    .expect("retried record")
                    .archived_attempts
                    .len(),
                1,
                "retry deduplicates an already installed pending archive"
            );
        }
    }

    #[test]
    fn current_and_baseline_attempt_markers_cannot_be_removed_replaced_or_reused() {
        let tmp = TempDir::new().expect("temporary state directory");
        let (template, spend, psbt) = fixture();
        let mut store = SavedPsbtStore::load(tmp.path().join("saved_psbts.json"));
        let id = store
            .create(template, spend, next_attempt(&psbt, 1))
            .expect("marked initial attempt")
            .record_id;
        store
            .reset(&id, |_, _| Ok(next_attempt(&psbt, 2)))
            .expect("second marked attempt");
        let active = store.get(&id).expect("active record").clone();
        for invalid in [v2(&psbt), next_attempt(&psbt, 1), next_attempt(&psbt, 3)] {
            assert!(
                store.update(&id, &invalid).is_err(),
                "current cannot change its attempt marker"
            );
            assert_eq!(
                store
                    .get(&id)
                    .expect("unchanged record")
                    .current_psbt_v2_hex,
                active.current_psbt_v2_hex,
                "rejected current marker leaves exact active bytes unchanged"
            );
            let mut changed = active.clone();
            changed.baseline_psbt_v2_hex = invalid.clone();
            assert!(
                validate_record(&changed).is_err(),
                "baseline cannot disagree with current marker"
            );
            changed = active.clone();
            changed.archived_attempts[0].current_psbt_v2_hex = invalid;
            if changed.archived_attempts[0].current_psbt_v2_hex
                != active.archived_attempts[0].current_psbt_v2_hex
            {
                assert!(
                    validate_record(&changed).is_err(),
                    "archived current must match its own baseline marker"
                );
            }
        }
        let mut reused = active.clone();
        reused.current_psbt_v2_hex = next_attempt(&psbt, 1);
        reused.baseline_psbt_v2_hex = reused.current_psbt_v2_hex.clone();
        assert!(
            validate_record(&reused)
                .expect_err("old marker reused by current")
                .contains("reuses"),
            "changing both current and baseline cannot revive an archived attempt"
        );
        let error = store
            .reset(&id, |current, _| Ok(current.to_string()))
            .expect_err("reset must create a fresh marker");
        assert!(
            error.contains("fresh"),
            "same-marker reset is rejected: {error}"
        );
    }

    #[test]
    fn repeated_resets_archive_exact_progress_and_attempt_baselines_across_reloads() {
        let tmp = TempDir::new().expect("temporary state directory");
        let path = tmp.path().join("saved_psbts.json");
        let (template, spend, psbt) = fixture();
        let baseline = v2(&psbt);
        let mut store = SavedPsbtStore::load(path.clone());
        let id = store
            .create(template, spend, baseline.clone())
            .expect("saved record")
            .record_id;
        let first = signing_progress(&psbt, 1);
        store.update(&id, &first).expect("round progress");
        let second_baseline = next_attempt(&psbt, 2);
        store
            .reset(&id, |current, original| {
                assert_eq!(
                    current, first,
                    "retirement receives exact accumulated progress"
                );
                assert_eq!(
                    original, baseline,
                    "reset uses original transaction baseline"
                );
                let disk = SavedPsbtStore::load(path.clone());
                let record = disk.get(&id).expect("archive persisted before retirement");
                assert!(
                    record.reset_pending,
                    "crash during retirement fences the old attempt"
                );
                assert_eq!(
                    record.archived_attempts[0].current_psbt_v2_hex, first,
                    "archive precedes retirement"
                );
                Ok(second_baseline.clone())
            })
            .expect("first reset");
        let mut store = SavedPsbtStore::load(path.clone());
        let second = signing_progress(&decode_psbt(&second_baseline).expect("second attempt"), 2);
        store.update(&id, &second).expect("second progress");
        let third_baseline = next_attempt(&psbt, 3);
        store
            .reset(&id, |_, _| Ok(third_baseline.clone()))
            .expect("second reset");
        let reloaded = SavedPsbtStore::load(path);
        assert!(
            reloaded.error().is_empty(),
            "valid attempt history: {}",
            reloaded.error()
        );
        let record = reloaded
            .get(&id)
            .expect("same transaction record after restart");
        assert_eq!(
            record.original_psbt_v2_hex, baseline,
            "initial baseline never changes"
        );
        assert_eq!(
            record.attempt_id, 2,
            "same TXID has three distinct attempt identities"
        );
        assert_eq!(
            record.current_psbt_v2_hex, third_baseline,
            "new attempt is active after restart"
        );
        assert_eq!(
            record.baseline_psbt_v2_hex, third_baseline,
            "active baseline is retained"
        );
        assert!(!record.reset_pending, "successful reset clears the fence");
        assert_eq!(
            record.archived_attempts.len(),
            2,
            "both prior attempts survive"
        );
        let a = &record.archived_attempts[0];
        assert_eq!(a.attempt_id, 0, "first identity");
        assert_eq!(a.baseline_psbt_v2_hex, baseline, "initial attempt baseline");
        assert_eq!(
            a.current_psbt_v2_hex, first,
            "exact first nonce, partial signature and unknown bytes"
        );
        let b = &record.archived_attempts[1];
        assert_eq!(b.attempt_id, 1, "second identity");
        assert_eq!(
            b.baseline_psbt_v2_hex, second_baseline,
            "second attempt baseline"
        );
        assert_eq!(
            b.current_psbt_v2_hex, second,
            "exact second nonce, partial signature and unknown bytes"
        );
    }

    #[test]
    fn archive_write_failure_never_invokes_retirement_or_loses_progress() {
        let tmp = TempDir::new().expect("temporary state directory");
        let path = tmp.path().join("saved_psbts.json");
        let (template, spend, psbt) = fixture();
        let mut store = SavedPsbtStore::load(path.clone());
        let id = store
            .create(template, spend, v2(&psbt))
            .expect("saved record")
            .record_id;
        let progress = signing_progress(&psbt, 1);
        store.update(&id, &progress).expect("progress");
        let before = fs::read(&path).expect("persisted progress");
        fs::create_dir(path.with_extension("tmp")).expect("block atomic archive write");
        let mut calls = 0;
        let error = store
            .reset(&id, |_, _| {
                calls += 1;
                Ok(next_attempt(&psbt, 2))
            })
            .expect_err("archive failure stops reset");
        assert!(
            error.contains("cannot archive"),
            "actionable archive failure: {error}"
        );
        assert_eq!(calls, 0, "no nonce retirement on failed archive");
        let record = store.get(&id).expect("unchanged active record");
        assert_eq!(
            record.current_psbt_v2_hex, progress,
            "pre-reset progress remains active"
        );
        assert!(
            record.archived_attempts.is_empty(),
            "failed archive rolled back in memory"
        );
        assert!(
            !record.reset_pending,
            "archive failure does not retire or fence a usable attempt"
        );
        assert_eq!(
            fs::read(&path).expect("old persisted progress"),
            before,
            "disk bytes unchanged"
        );
        fs::remove_dir(path.with_extension("tmp")).expect("unblock archive");
        store
            .reset(&id, |_, _| Ok(next_attempt(&psbt, 2)))
            .expect("retry reset");
        assert_eq!(
            SavedPsbtStore::load(path)
                .get(&id)
                .expect("retried record")
                .archived_attempts
                .len(),
            1,
            "retry archives once"
        );
    }

    #[test]
    fn retirement_failure_keeps_current_fenced_and_retry_deduplicates_archive() {
        let tmp = TempDir::new().expect("temporary state directory");
        let path = tmp.path().join("saved_psbts.json");
        let (template, spend, psbt) = fixture();
        let mut store = SavedPsbtStore::load(path.clone());
        let id = store
            .create(template, spend, v2(&psbt))
            .expect("saved record")
            .record_id;
        let progress = signing_progress(&psbt, 1);
        store.update(&id, &progress).expect("progress");
        assert_eq!(
            store
                .reset(&id, |_, _| Err("retirement write failed".into()))
                .expect_err("retirement failure"),
            "retirement write failed",
            "original error propagates"
        );
        let mut store = SavedPsbtStore::load(path.clone());
        let record = store.get(&id).expect("pending reset after restart");
        assert!(
            record.reset_pending,
            "partly retired attempt cannot resume signing"
        );
        assert_eq!(
            record.current_psbt_v2_hex, progress,
            "failed reset keeps current bytes"
        );
        assert_eq!(
            record.attempt_id, 0,
            "failed reset does not advance identity"
        );
        assert!(
            store.update(&id, &progress).is_err(),
            "cannot overwrite pending snapshot"
        );
        store
            .reset(&id, |_, _| Err("still blocked".into()))
            .expect_err("second failure");
        assert_eq!(
            store
                .get(&id)
                .expect("pending record")
                .archived_attempts
                .len(),
            1,
            "retry does not duplicate the same attempt"
        );
        let next = next_attempt(&psbt, 2);
        store
            .reset(&id, |_, _| Ok(next.clone()))
            .expect("complete retirement");
        let disk = SavedPsbtStore::load(path);
        let record = disk.get(&id).expect("new active record");
        assert_eq!(
            record.archived_attempts.len(),
            1,
            "one archive for the retired identity"
        );
        assert_eq!(
            record.current_psbt_v2_hex, next,
            "new attempt published only on success"
        );
        assert!(
            !record.reset_pending,
            "successful retry clears pending fence"
        );
    }

    #[test]
    fn new_attempt_save_failure_preserves_durable_archive_and_pending_fence() {
        let tmp = TempDir::new().expect("temporary state directory");
        let path = tmp.path().join("saved_psbts.json");
        let (template, spend, psbt) = fixture();
        let mut store = SavedPsbtStore::load(path.clone());
        let id = store
            .create(template, spend, v2(&psbt))
            .expect("saved record")
            .record_id;
        let progress = signing_progress(&psbt, 1);
        store.update(&id, &progress).expect("progress");
        let next = next_attempt(&psbt, 2);
        store
            .reset(&id, |_, _| {
                fs::create_dir(path.with_extension("tmp"))
                    .expect("fail active attempt save after archive");
                Ok(next.clone())
            })
            .expect("new attempt remains usable in memory");
        assert!(
            !store.error().is_empty(),
            "ordinary active save failure is visible"
        );
        assert_eq!(
            store.get(&id).expect("in memory").current_psbt_v2_hex,
            next,
            "new attempt stays usable"
        );
        let disk = SavedPsbtStore::load(path.clone());
        let record = disk.get(&id).expect("durable archive");
        assert!(
            record.reset_pending,
            "restart cannot sign a retired current attempt"
        );
        assert_eq!(
            record.archived_attempts[0].current_psbt_v2_hex, progress,
            "pre-reset bytes stay durable"
        );
        fs::remove_dir(path.with_extension("tmp")).expect("unblock active save");
        assert!(
            store.persist().is_empty(),
            "ordinary save retry persists the active attempt"
        );
        assert_eq!(
            SavedPsbtStore::load(path)
                .get(&id)
                .expect("new active attempt")
                .current_psbt_v2_hex,
            next,
            "retry saves the new attempt"
        );
    }

    #[test]
    fn legacy_record_without_history_can_reset_without_losing_its_baseline() {
        let tmp = TempDir::new().expect("temporary state directory");
        let path = tmp.path().join("saved_psbts.json");
        let (template, spend, psbt) = fixture();
        let mut store = SavedPsbtStore::load(path.clone());
        let id = store
            .create(template, spend, v2(&psbt))
            .expect("saved record")
            .record_id;
        let text = state_crypt::read_state(&path)
            .expect("read state")
            .expect("existing state");
        let mut json: serde_json::Value = serde_json::from_str(&text).expect("JSON state");
        for field in [
            "attempt_id",
            "baseline_psbt_v2_hex",
            "archived_attempts",
            "reset_pending",
        ] {
            json["records"][0]
                .as_object_mut()
                .expect("record object")
                .remove(field);
        }
        state_crypt::write_state(&path, &json.to_string()).expect("legacy record");
        let mut store = SavedPsbtStore::load(path);
        assert!(
            store.error().is_empty(),
            "legacy record loads: {}",
            store.error()
        );
        store
            .reset(&id, |_, _| Ok(next_attempt(&psbt, 2)))
            .expect("legacy reset");
        assert_eq!(
            store.get(&id).expect("upgraded record").archived_attempts[0].baseline_psbt_v2_hex,
            v2(&psbt),
            "legacy baseline retained exactly"
        );
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
        assert!(
            store
                .reset(&saved.record_id, |_, _| panic!(
                    "unreadable storage must not retire nonces"
                ))
                .is_err(),
            "reset is blocked until the archive can be persisted"
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
        assert!(
            store
                .reset(&saved.record_id, |_, _| panic!(
                    "finalized transactions must not retire nonces"
                ))
                .is_err(),
            "finalized saved records cannot be reset"
        );
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
        store
            .broadcast(tx.compute_txid())
            .expect("persist broadcast");
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
