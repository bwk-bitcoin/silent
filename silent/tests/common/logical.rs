use std::{path::PathBuf, process::Command, sync::Arc};

use base64::{prelude::BASE64_STANDARD, Engine};
use bitcoin::hashes::{sha256, Hash};
use bwk::{
    bwk_electrum::persistence::{BackendIdentity, BackendKind},
    persist::backend::PersistenceBackend,
};
use serde_json::{json, Value};
use silent::{
    storage::{storage_request, storage_session_open, storage_session_revoke},
    Config,
};

pub fn path(config: &Config, file: &str) -> String {
    format!(
        "accounts/{}/{file}",
        sha256::Hash::hash(config.account_name.as_bytes())
    )
}

pub fn call(config: &Config, mut request: Value) -> Value {
    let session = storage_session_open(config.plugin_id.clone(), String::new(), false);
    let opened: Value = serde_json::from_str(&storage_request(
        session,
        json!({"id":"open", "op":"open", "storage":"private"}).to_string(),
    ))
    .expect("open reply");
    assert_eq!(opened["error"], "", "open logical storage: {opened}");
    request["id"] = "operation".into();
    request["storage"] = opened["storage"].clone();
    let reply = storage_request(session, request.to_string());
    storage_session_revoke(session);
    let reply: Value = serde_json::from_str(&reply).expect("logical operation reply");
    assert_eq!(reply["error"], "", "logical operation: {reply}");
    reply
}

pub fn read(config: &Config, file: &str) -> Vec<u8> {
    let reply = call(config, json!({"op":"read", "path":path(config, file)}));
    BASE64_STANDARD
        .decode(reply["data"].as_str().expect("file data"))
        .expect("whole file bytes")
}

pub fn write(config: &Config, file: &str, bytes: &[u8]) {
    call(
        config,
        json!({"op":"write", "path":path(config, file), "data":BASE64_STANDARD.encode(bytes), "mode":"upsert"}),
    );
}

pub fn rename(config: &Config, from: &str, to: &str) {
    call(
        config,
        json!({"op":"rename", "path":path(config, from), "to":path(config, to)}),
    );
}

pub fn block_file(config: &Config, file: &str) {
    rename(config, file, &format!("{file}.backup"));
    call(config, json!({"op":"mkdir", "path":path(config, file)}));
}

pub fn restore_file(config: &Config, file: &str) {
    call(config, json!({"op":"remove", "path":path(config, file)}));
    rename(config, &format!("{file}.backup"), file);
}

pub fn backend(config: &Config, kind: BackendKind) -> Arc<dyn PersistenceBackend> {
    let factory = if config.plugin_id == "sp" {
        config
            .to_sp_config()
            .expect("SP metadata")
            .backend_factory
            .expect("logical SP factory")
    } else {
        config
            .to_musig_config()
            .expect("MuSig metadata")
            .scanner
            .backend_factory
            .expect("logical MuSig factory")
    };
    let account = match kind {
        BackendKind::SubAccount(index) => format!("{}-sub-{index}", config.account_name),
        _ => config.account_name.clone(),
    };
    factory
        .open(&BackendIdentity {
            account,
            network: config.network.into(),
            kind,
        })
        .expect("open logical byte-row backend")
}

pub fn root(config: &Config) -> PathBuf {
    config.data_dir.join(&config.plugin_id)
}

pub fn isolated_process(test: &str) -> bool {
    if std::env::var("SILENT_LAYOUT_TEST").as_deref() == Ok(test) {
        return false;
    }
    let status = Command::new(std::env::current_exe().expect("test executable"))
        .args(["--exact", test, "--test-threads=1"])
        .env("SILENT_LAYOUT_TEST", test)
        .status()
        .expect("isolated layout failure test");
    assert!(status.success(), "isolated {test}: {status}");
    true
}

#[cfg(unix)]
pub struct ReadOnlyStorage {
    root: PathBuf,
    permissions: std::fs::Permissions,
}

#[cfg(unix)]
impl ReadOnlyStorage {
    pub fn new(config: &Config) -> Self {
        use std::os::unix::fs::PermissionsExt;
        let root = root(config);
        let permissions = std::fs::metadata(&root).expect("plugin root").permissions();
        std::fs::set_permissions(&root, std::fs::Permissions::from_mode(0o500))
            .expect("deny logical publication");
        Self { root, permissions }
    }
}

#[cfg(unix)]
impl Drop for ReadOnlyStorage {
    fn drop(&mut self) {
        std::fs::set_permissions(&self.root, self.permissions.clone())
            .expect("restore logical publication");
    }
}
