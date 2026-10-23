//! Plugin removal against a second real process. Each test spawns this test binary as a
//! scenario child, which owns its own data directory (`set_datadir` is once per process)
//! and spawns the binary again as a worker: two processes sharing one data directory,
//! the worker driven with JSON lines on its stdin and answering on its stdout.

use std::{
    collections::BTreeMap,
    io::{BufRead, BufReader, Read, Write},
    path::{Path, PathBuf},
    process::{Child, ChildStdin, Command, ExitStatus, Stdio},
    sync::mpsc::{self, Receiver, RecvTimeoutError},
    thread,
    time::{Duration, Instant},
};

use bitcoin::hashes::{sha256, Hash};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use temp_dir::TempDir;

use crate::{
    app_state::{
        advance_plugin_removal, app_begin_plugin_removal, app_language, app_plugin_access_history,
        app_plugin_access_request, app_plugin_grants, app_plugin_history, app_set_language,
        app_set_plugin_access, plugin_removal_phase, AppState, RemovalPhase, StoredGrant,
    },
    config::{datadir, set_datadir},
    ffi::{PluginAccess, PluginGrant},
    plugin_admission::Compatibility,
    plugin_bundle,
    state_crypt::{self, DurableWriteStage},
    storage,
};

const ROLE: &str = "SILENT_REMOVAL_PROCESS_ROLE";
const DATADIR: &str = "SILENT_REMOVAL_PROCESS_DATADIR";
const ENTRY: &str = "removal_process_tests::process_role";
const WORKER: &str = "worker";
const REPLY: &str = "@@";
const RESUME: &str = "resume";
const PLUGIN: &str = "removal-probe";
/// "data", base64.
const DATA: &str = "ZGF0YQ==";
const IID: &str = "dev.silent.IPlugin/9.0";
const LIBRARY: &str = "libremoval.so";
const UNIX_HOST: u16 = 3;
const UNIX_REGULAR: u32 = 0o100_644;
/// Any answer the other process owes, or a whole scenario, comes within these.
const TIMEOUT: Duration = Duration::from_secs(60);
const SCENARIO_TIMEOUT: Duration = Duration::from_secs(300);
/// How long an app-state transaction must stay blocked behind the paused one.
const BLOCKED_FOR: Duration = Duration::from_millis(500);
const POLL: Duration = Duration::from_millis(20);

/// What the scenario asks the worker, one JSON line each.
#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "cmd", rename_all = "snake_case")]
enum WorkerCommand {
    /// Opens a storage session as the host authenticated the plugin, and its handle.
    Open {
        name: String,
    },
    /// Sends `request` on the session `name`, its storage handle filled in.
    Request {
        name: String,
        request: Value,
    },
    Language {
        locale: String,
        pause: bool,
    },
    /// Loads the app state now and keeps it.
    Snapshot,
    /// Answers the snapshot kept, never reloaded.
    Held,
    Install {
        path: PathBuf,
        pause: bool,
    },
    /// Reads and keeps, as `name`, the plugin's answers and storage epoch, as the host
    /// prompt does before it opens.
    GrantRequest {
        name: String,
    },
    /// Reads and keeps, as `name`, the plugin's answers and storage epoch, as the
    /// Settings Access view does when it opens.
    GrantHistory {
        name: String,
    },
    /// Allows Keys on the answers kept as `name`, saved against their epoch.
    GrantSave {
        name: String,
    },
}

fn spawn_role(role: &str, data_dir: &Path) -> Child {
    Command::new(std::env::current_exe().expect("test binary path"))
        .args([
            ENTRY,
            "--exact",
            "--ignored",
            "--nocapture",
            "--test-threads=1",
            "-q",
        ])
        .env(ROLE, role)
        .env(DATADIR, data_dir)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("spawn child test process")
}

/// Waits for `child` at most `timeout`, killing it past that.
fn wait_bounded(child: &mut Child, timeout: Duration) -> ExitStatus {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(status) = child.try_wait().expect("poll child process") {
            return status;
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            panic!("child process did not exit within {timeout:?}");
        }
        thread::sleep(POLL);
    }
}

fn collect(mut pipe: impl Read + Send + 'static) -> thread::JoinHandle<String> {
    thread::spawn(move || {
        let mut text = String::new();
        let _ = pipe.read_to_string(&mut text);
        text
    })
}

/// Runs the scenario `role` in its own process and data directory.
fn run_scenario(role: &str) {
    let root = TempDir::new().expect("scenario directory");
    let mut child = spawn_role(role, &root.path().join("state"));
    drop(child.stdin.take());
    let stdout = collect(child.stdout.take().expect("scenario stdout"));
    let stderr = collect(child.stderr.take().expect("scenario stderr"));
    let status = wait_bounded(&mut child, SCENARIO_TIMEOUT);
    let stdout = stdout.join().expect("scenario stdout reader");
    let stderr = stderr.join().expect("scenario stderr reader");
    assert!(
        status.success(),
        "scenario '{role}' failed: {status}\n--- stdout\n{stdout}\n--- stderr\n{stderr}"
    );
    assert!(
        stdout.contains("1 passed"),
        "scenario '{role}' did not run its entry point:\n{stdout}"
    );
}

/// The entry point of every child process; a no-op when run directly.
#[test]
#[ignore = "entry point of the child processes the tests below spawn"]
fn process_role() {
    let Ok(role) = std::env::var(ROLE) else {
        return;
    };
    set_datadir(PathBuf::from(
        std::env::var(DATADIR).expect("child data directory"),
    ));
    state_crypt::unlock_for_tests();
    match role.as_str() {
        WORKER => worker(),
        "stale-preference" => stale_preference_save_blocks_removal_phases(),
        "stale-snapshot" => stale_snapshot_setter_keeps_private_data_phase(),
        #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
        "same-hash-reinstall" => old_session_stays_revoked_across_same_hash_reinstall(),
        #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
        "paused-install" => paused_install_publishes_before_removal_is_admitted(),
        #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
        "pending-removal-install" => pending_removal_refuses_install_from_another_process(),
        #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
        "stale-grant" => held_grant_reads_cannot_save_across_removal_and_same_hash_reinstall(),
        other => panic!("unknown child role '{other}'"),
    }
}

fn report(reply: &Value) {
    println!("{REPLY}{reply}");
}

/// The worker process: one reply per command, until its stdin closes.
fn worker() {
    let mut host = WorkerHost::default();
    let mut line = String::new();
    loop {
        line.clear();
        if std::io::stdin()
            .read_line(&mut line)
            .expect("read worker command")
            == 0
        {
            return;
        }
        let command = serde_json::from_str(&line).expect("worker command JSON");
        report(&host.run(command));
    }
}

/// Pauses the next durable write of this thread once its temp is synced, before the
/// rename publishes it, until the scenario sends `RESUME`.
fn pause_next_save() {
    state_crypt::on_durable_write_at(DurableWriteStage::TempSynced, || {
        report(&json!({"paused": true}));
        let mut line = String::new();
        std::io::stdin()
            .read_line(&mut line)
            .expect("read resume line");
        assert_eq!(line.trim(), RESUME, "only a resume ends the pause");
    });
}

fn removal_view(state: &AppState) -> Value {
    json!({
        "phase": state.removing_plugins.get(PLUGIN),
        "epoch": state.storage_epochs.get(PLUGIN),
    })
}

#[derive(Default)]
struct WorkerHost {
    /// Each open session and its storage handle, by name.
    sessions: BTreeMap<String, (u64, String)>,
    snapshot: Option<AppState>,
    /// Each grant request or history kept: the storage epoch it was read with and the
    /// answers for the plugin's library, by name.
    grant_reads: BTreeMap<String, (u64, PluginAccess)>,
}

impl WorkerHost {
    fn run(&mut self, command: WorkerCommand) -> Value {
        match command {
            WorkerCommand::Open { name } => self.open(name),
            WorkerCommand::Request { name, request } => self.request(&name, request),
            WorkerCommand::Language { locale, pause } => {
                if pause {
                    pause_next_save();
                }
                app_set_language(locale);
                json!({"done": true})
            }
            WorkerCommand::Snapshot => {
                let state = AppState::try_load().expect("worker app state snapshot");
                let view = removal_view(&state);
                self.snapshot = Some(state);
                view
            }
            WorkerCommand::Held => {
                removal_view(self.snapshot.as_ref().expect("a snapshot was taken"))
            }
            WorkerCommand::Install { path, pause } => {
                if pause {
                    pause_next_save();
                }
                match plugin_bundle::install(&path, &host()) {
                    Ok(id) => json!({"installed": id}),
                    Err(error) => json!({"error": error}),
                }
            }
            WorkerCommand::GrantRequest { name } => {
                let request = app_plugin_access_request(PLUGIN.into(), plugin_hash());
                let reply = json!({
                    "error": request.error,
                    "epoch": request.storage_epoch,
                    "keys": grant_name(request.access.keys),
                    "state": grant_name(request.access.state),
                });
                if request.is_ok {
                    self.grant_reads
                        .insert(name, (request.storage_epoch, request.access));
                }
                reply
            }
            WorkerCommand::GrantHistory { name } => {
                let history = app_plugin_access_history(PLUGIN.into());
                let reply = json!({
                    "error": history.error,
                    "epoch": history.storage_epoch,
                    "entries": history.entries.len(),
                });
                if let Some(entry) = history
                    .entries
                    .into_iter()
                    .find(|entry| entry.hash == plugin_hash())
                {
                    self.grant_reads
                        .insert(name, (history.storage_epoch, entry));
                }
                reply
            }
            WorkerCommand::GrantSave { name } => {
                let (epoch, access) = self.grant_reads.get(&name).expect("a grant read kept");
                let access = PluginAccess {
                    keys: PluginGrant::Allowed,
                    ..access.clone()
                };
                json!({"error": app_set_plugin_access(access, *epoch)})
            }
        }
    }

    fn open(&mut self, name: String) -> Value {
        let session = storage::storage_session_open(PLUGIN.into(), plugin_hash(), true);
        if session == 0 {
            return json!({"session": false});
        }
        let opened: Value = serde_json::from_str(&storage::storage_request(
            session,
            json!({"id": "open", "op": "open", "storage": "private"}).to_string(),
        ))
        .expect("open response JSON");
        let handle = opened["storage"].as_str().unwrap_or_default().to_string();
        self.sessions.insert(name, (session, handle));
        json!({"session": true, "error": opened["error"]})
    }

    fn request(&self, name: &str, mut request: Value) -> Value {
        let (session, handle) = self.sessions.get(name).expect("an opened session");
        request["storage"] = handle.clone().into();
        serde_json::from_str(&storage::storage_request(*session, request.to_string()))
            .expect("storage response JSON")
    }
}

/// The worker process, seen from the scenario.
struct Worker {
    child: Child,
    stdin: Option<ChildStdin>,
    replies: Receiver<Value>,
}

impl Worker {
    fn spawn() -> Self {
        let mut child = spawn_role(WORKER, &datadir());
        let stdin = child.stdin.take();
        let stdout = child.stdout.take().expect("worker stdout");
        let mut stderr = child.stderr.take().expect("worker stderr");
        thread::spawn(move || {
            let _ = std::io::copy(&mut stderr, &mut std::io::stderr());
        });
        let (sender, replies) = mpsc::channel();
        thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let Ok(line) = line else {
                    return;
                };
                if let Some(reply) = line.strip_prefix(REPLY) {
                    let reply = serde_json::from_str(reply).expect("worker reply JSON");
                    if sender.send(reply).is_err() {
                        return;
                    }
                }
            }
        });
        Worker {
            child,
            stdin,
            replies,
        }
    }

    fn send_line(&mut self, line: &str) {
        let stdin = self.stdin.as_mut().expect("worker stdin open");
        writeln!(stdin, "{line}").expect("write worker stdin");
        stdin.flush().expect("flush worker stdin");
    }

    fn send(&mut self, command: WorkerCommand) {
        let line = serde_json::to_string(&command).expect("encode worker command");
        self.send_line(&line);
    }

    fn reply(&self) -> Value {
        self.replies
            .recv_timeout(TIMEOUT)
            .expect("worker reply within the timeout")
    }

    fn call(&mut self, command: WorkerCommand) -> Value {
        self.send(command);
        self.reply()
    }

    fn open(&mut self, name: &str) -> Value {
        self.call(WorkerCommand::Open { name: name.into() })
    }

    fn storage(&mut self, name: &str, request: Value) -> Value {
        self.call(WorkerCommand::Request {
            name: name.into(),
            request,
        })
    }

    fn write(&mut self, name: &str, path: &str) -> Value {
        self.storage(
            name,
            json!({"id": path, "op": "write", "path": path, "data": DATA, "mode": "upsert"}),
        )
    }

    fn read(&mut self, name: &str, path: &str) -> Value {
        self.storage(name, json!({"id": path, "op": "read", "path": path}))
    }

    fn list(&mut self, name: &str) -> Value {
        self.storage(name, json!({"id": "list", "op": "list", "path": ""}))
    }

    /// Starts a command whose save pauses before publication, the app-state lock held.
    fn pause_in(&mut self, command: WorkerCommand) {
        self.send(command);
        assert_eq!(
            self.reply(),
            json!({"paused": true}),
            "the worker paused in its save"
        );
    }

    fn resume(&mut self) {
        self.send_line(RESUME);
    }

    /// Closes the worker's stdin and checks it exits cleanly.
    fn finish(mut self) {
        drop(self.stdin.take());
        let status = wait_bounded(&mut self.child, TIMEOUT);
        assert!(status.success(), "worker exited with {status}");
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        if matches!(self.child.try_wait(), Ok(None)) {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

fn answer(id: &str, error: &str) -> Value {
    json!({"id": id, "error": error})
}

/// Runs `operation` in this process while the worker is paused in a save, the app-state
/// lock held: it must wait until the worker published and answered `published`.
fn blocked_by_paused_worker<T: Send + 'static>(
    worker: &mut Worker,
    published: Value,
    operation: impl FnOnce() -> T + Send + 'static,
) -> T {
    let (sender, outcome) = mpsc::channel();
    let racer = thread::spawn(move || {
        let _ = sender.send(operation());
    });
    assert!(
        matches!(
            outcome.recv_timeout(BLOCKED_FOR),
            Err(RecvTimeoutError::Timeout)
        ),
        "another process' app-state transaction cannot run inside the paused one"
    );
    worker.resume();
    assert_eq!(worker.reply(), published, "the paused worker published");
    let result = outcome
        .recv_timeout(TIMEOUT)
        .expect("the blocked transaction runs once the worker released the lock");
    racer.join().expect("racing transaction thread");
    result
}

fn library() -> Vec<u8> {
    let root = BTreeMap::from([(
        serde_cbor::Value::Integer(2),
        serde_cbor::Value::Text(IID.into()),
    )]);
    let mut record = b"QTMETADATA !\x01\x06\x08\x01".to_vec();
    record.extend(serde_cbor::to_vec(&root).expect("encode metadata fixture"));
    elf(&record)
}

fn plugin_hash() -> String {
    sha256::Hash::hash(&library()).to_string()
}

fn host() -> Compatibility {
    Compatibility {
        iid: IID.into(),
        target_os: std::env::consts::OS.into(),
        target_arch: std::env::consts::ARCH.into(),
        qt_major: 6,
        qt_minor: 8,
        arch_requirements: 1,
    }
}

fn grant_name(grant: PluginGrant) -> Value {
    serde_json::to_value(StoredGrant::from(grant)).expect("grant name")
}

fn grant() {
    let request = app_plugin_access_request(PLUGIN.into(), plugin_hash());
    assert!(request.is_ok, "grant request: {}", request.error);
    let saved = app_set_plugin_access(
        PluginAccess {
            state: PluginGrant::Allowed,
            ..request.access
        },
        request.storage_epoch,
    );
    assert_eq!(saved, "", "the Saved Data grant is saved");
    assert!(
        app_plugin_grants(plugin_hash()).state,
        "the Saved Data grant is published"
    );
}

fn phase() -> Option<RemovalPhase> {
    plugin_removal_phase(PLUGIN).expect("read removal phase")
}

fn epoch() -> Option<u64> {
    AppState::try_load()
        .expect("read app state")
        .storage_epochs
        .get(PLUGIN)
        .copied()
}

/// The real removal ends with a success, its journal gone and the epoch retained; once
/// regranted, the old session stays revoked and a fresh one finds an empty tree.
fn assert_removed_blank(worker: &mut Worker, old: &str) {
    assert_eq!(
        plugin_bundle::plugin_remove(PLUGIN.into()),
        "",
        "the real removal reports success"
    );
    assert_eq!(phase(), None, "the journal entry is finished");
    assert_eq!(epoch(), Some(1), "the storage epoch outlives the removal");
    assert!(
        app_plugin_history(PLUGIN.into()).is_empty(),
        "the removed plugin's grants are erased"
    );
    grant();
    assert_eq!(
        worker.write(old, "revived"),
        answer("revived", "revoked"),
        "a regrant never revives the old session"
    );
    assert_eq!(worker.open("fresh"), json!({"session": true, "error": ""}));
    assert_eq!(
        worker.list("fresh"),
        json!({"id": "list", "error": "", "entries": [], "next": ""}),
        "the private tree was really cleared"
    );
    assert_eq!(
        worker.read("fresh", "account"),
        answer("account", "not_found")
    );
}

/// A preference save paused after its load, its lock held, holds back the removal
/// checkpoints of another process, which then keep both changes.
fn stale_preference_save_blocks_removal_phases() {
    grant();
    let mut worker = Worker::spawn();
    assert_eq!(worker.open("old"), json!({"session": true, "error": ""}));
    assert_eq!(worker.write("old", "account"), answer("account", ""));

    worker.pause_in(WorkerCommand::Language {
        locale: "fr".into(),
        pause: true,
    });
    assert_eq!(phase(), None, "nothing is published during the pause");
    let begun = blocked_by_paused_worker(&mut worker, json!({"done": true}), || {
        app_begin_plugin_removal(PLUGIN.into())
    });
    assert_eq!(begun, "", "the removal begins after the preference save");
    assert_eq!(app_language(), "fr", "the begin kept the preference");
    assert_eq!(phase(), Some(RemovalPhase::Stopping));
    assert_eq!(
        worker.write("old", "flush"),
        answer("flush", ""),
        "a session opened before the removal may flush while stopping"
    );

    worker.pause_in(WorkerCommand::Language {
        locale: "de".into(),
        pause: true,
    });
    assert_eq!(phase(), Some(RemovalPhase::Stopping));
    let advanced = blocked_by_paused_worker(&mut worker, json!({"done": true}), || {
        advance_plugin_removal(PLUGIN, RemovalPhase::PrivateData)
    });
    assert_eq!(advanced, Ok(()));
    assert_eq!(
        app_language(),
        "de",
        "the erasure checkpoint kept the preference"
    );
    assert_eq!(
        phase(),
        Some(RemovalPhase::PrivateData),
        "the stale save never downgraded the journal"
    );
    assert_eq!(
        epoch(),
        Some(1),
        "the first erasure checkpoint rotates the epoch"
    );
    assert_eq!(
        worker.write("old", "late"),
        answer("late", "revoked"),
        "private writes are fenced once erasure is durable"
    );

    assert_removed_blank(&mut worker, "old");
    assert_eq!(app_language(), "de");
    worker.finish();
}

/// A worker holding a snapshot read before the erasure checkpoint saves a preference
/// with the real setter: the setter reloads under the lock and keeps the checkpoint.
fn stale_snapshot_setter_keeps_private_data_phase() {
    grant();
    let mut worker = Worker::spawn();
    assert_eq!(worker.open("old"), json!({"session": true, "error": ""}));
    assert_eq!(worker.write("old", "account"), answer("account", ""));
    assert_eq!(app_begin_plugin_removal(PLUGIN.into()), "");
    let stale = json!({"phase": "stopping", "epoch": null});
    assert_eq!(worker.call(WorkerCommand::Snapshot), stale);

    advance_plugin_removal(PLUGIN, RemovalPhase::PrivateData).expect("erasure checkpoint");
    assert_eq!(
        worker.call(WorkerCommand::Language {
            locale: "fr".into(),
            pause: false,
        }),
        json!({"done": true})
    );
    assert_eq!(
        worker.call(WorkerCommand::Held),
        stale,
        "the worker still holds the snapshot from before the checkpoint"
    );
    assert_eq!(app_language(), "fr");
    assert_eq!(
        phase(),
        Some(RemovalPhase::PrivateData),
        "the setter cannot downgrade the erasure phase"
    );
    assert_eq!(
        epoch(),
        Some(1),
        "the setter cannot erase the rotated epoch"
    );
    assert_eq!(
        worker.write("old", "late"),
        answer("late", "revoked"),
        "private writes are fenced once erasure is durable"
    );

    assert_removed_blank(&mut worker, "old");
    assert_eq!(app_language(), "fr");
    worker.finish();
}

#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn bundle(version: &str) -> PathBuf {
    let library = library();
    let found =
        crate::plugin_admission::inspect(&library, &host(), None).expect("fixture admission");
    let manifest = json!({
        "format_version": 1,
        "id": PLUGIN,
        "name": "Removal Probe",
        "version": version,
        "description": "A plugin removed while another process runs",
        "author": "Silent",
        "capabilities": [],
        "entry": {
            "kind": "native",
            "library": format!("payload/{LIBRARY}"),
            "compatibility": found,
        },
        "translations": [],
    });
    let archive = zip(&[
        ("manifest.json", manifest.to_string().as_bytes()),
        (&format!("payload/{LIBRARY}"), &library),
    ]);
    let dir = datadir()
        .parent()
        .expect("scenario directory")
        .join("bundles");
    std::fs::create_dir_all(&dir).expect("bundle directory");
    let path = dir.join(version);
    std::fs::write(&path, archive).expect("write bundle fixture");
    path
}

#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn installed_version() -> Option<Value> {
    let listed = plugin_bundle::list();
    assert!(listed.len() <= 1, "one plugin at most: {listed:?}");
    listed.first().map(|sidecar| {
        serde_json::from_str::<Value>(sidecar).expect("sidecar JSON")["version"].clone()
    })
}

#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn sealed_bundles() -> Vec<(String, Vec<u8>)> {
    let mut files: Vec<_> = std::fs::read_dir(datadir().join(plugin_bundle::PLUGINS_DIR))
        .expect("plugins directory")
        .map(|entry| {
            let entry = entry.expect("plugins entry");
            (
                entry.file_name().to_string_lossy().to_string(),
                std::fs::read(entry.path()).expect("read sealed bundle"),
            )
        })
        .collect();
    files.sort();
    files
}

/// A session opened before the removal stays revoked through the removal, the
/// reinstall of the same bundle and the same grant; an upgrade keeps a fresh session.
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn old_session_stays_revoked_across_same_hash_reinstall() {
    let first = bundle("0.2.0");
    assert_eq!(plugin_bundle::install(&first, &host()), Ok(PLUGIN.into()));
    grant();
    let mut worker = Worker::spawn();
    assert_eq!(worker.open("old"), json!({"session": true, "error": ""}));
    assert_eq!(worker.write("old", "before"), answer("before", ""));

    assert_eq!(app_begin_plugin_removal(PLUGIN.into()), "");
    assert_eq!(
        worker.write("old", "flush"),
        answer("flush", ""),
        "the existing session may flush while stopping"
    );
    assert_eq!(
        worker.open("second"),
        json!({"session": false}),
        "no new session while the removal is pending"
    );
    advance_plugin_removal(PLUGIN, RemovalPhase::PrivateData).expect("erasure checkpoint");
    assert_eq!(
        worker.write("old", "late"),
        answer("late", "revoked"),
        "the erasure checkpoint revokes the old session in its own process"
    );
    assert_eq!(plugin_bundle::plugin_remove(PLUGIN.into()), "");
    assert_eq!(phase(), None);
    assert_eq!(epoch(), Some(1));
    assert_eq!(installed_version(), None, "the bundle is removed");
    assert_eq!(worker.read("old", "before"), answer("before", "revoked"));

    assert_eq!(plugin_bundle::install(&first, &host()), Ok(PLUGIN.into()));
    grant();
    assert_eq!(
        worker.write("old", "revived"),
        answer("revived", "revoked"),
        "the same bundle and grant never revive the old session"
    );
    assert_eq!(worker.read("old", "before"), answer("before", "revoked"));
    assert_eq!(worker.open("fresh"), json!({"session": true, "error": ""}));
    assert_eq!(
        worker.list("fresh"),
        json!({"id": "list", "error": "", "entries": [], "next": ""}),
        "the reinstalled plugin starts blank"
    );
    assert_eq!(
        worker.read("fresh", "before"),
        answer("before", "not_found")
    );
    assert_eq!(worker.read("fresh", "flush"), answer("flush", "not_found"));
    assert_eq!(worker.write("fresh", "after"), answer("after", ""));

    assert_eq!(
        plugin_bundle::install(&bundle("0.3.0"), &host()),
        Ok(PLUGIN.into())
    );
    assert_eq!(installed_version(), Some(json!("0.3.0")));
    assert_eq!(phase(), None, "an upgrade is not a removal");
    assert_eq!(epoch(), Some(1), "an upgrade keeps the storage epoch");
    assert_eq!(
        worker.read("fresh", "after"),
        json!({"id": "after", "error": "", "data": DATA}),
        "the session open across the upgrade keeps its store"
    );
    assert_eq!(worker.write("fresh", "next"), answer("next", ""));
    assert_eq!(
        worker.open("upgraded"),
        json!({"session": true, "error": ""})
    );
    assert_eq!(
        worker.read("upgraded", "after"),
        json!({"id": "after", "error": "", "data": DATA})
    );
    worker.finish();
}

/// An install paused before its rename, the app-state lock held, publishes before a
/// removal begun by another process is admitted.
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn paused_install_publishes_before_removal_is_admitted() {
    let path = bundle("0.2.0");
    let mut worker = Worker::spawn();
    worker.pause_in(WorkerCommand::Install { path, pause: true });
    assert_eq!(
        installed_version(),
        None,
        "the paused install is unpublished"
    );
    let begun = blocked_by_paused_worker(&mut worker, json!({"installed": PLUGIN}), || {
        app_begin_plugin_removal(PLUGIN.into())
    });
    assert_eq!(begun, "");
    assert_eq!(phase(), Some(RemovalPhase::Stopping));
    assert_eq!(
        installed_version(),
        Some(json!("0.2.0")),
        "the install published first"
    );
    assert_eq!(plugin_bundle::plugin_remove(PLUGIN.into()), "");
    assert_eq!(
        installed_version(),
        None,
        "the removal admitted after removes it"
    );
    assert_eq!(phase(), None);
    worker.finish();
}

/// While a removal is pending, an install from another process is refused and
/// publishes nothing; once the removal is done it installs.
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn pending_removal_refuses_install_from_another_process() {
    assert_eq!(
        plugin_bundle::install(&bundle("0.2.0"), &host()),
        Ok(PLUGIN.into())
    );
    assert_eq!(app_begin_plugin_removal(PLUGIN.into()), "");
    let before = sealed_bundles();
    let next = bundle("0.3.0");
    let mut worker = Worker::spawn();
    assert_eq!(
        worker.call(WorkerCommand::Install {
            path: next.clone(),
            pause: false,
        }),
        json!({"error": "plugin removal must be completed before reinstalling"})
    );
    assert_eq!(
        sealed_bundles(),
        before,
        "the refused install wrote nothing, not even a temp"
    );
    assert_eq!(installed_version(), Some(json!("0.2.0")));
    assert_eq!(phase(), Some(RemovalPhase::Stopping));

    assert_eq!(plugin_bundle::plugin_remove(PLUGIN.into()), "");
    assert_eq!(installed_version(), None);
    assert_eq!(
        worker.call(WorkerCommand::Install {
            path: next,
            pause: false,
        }),
        json!({"installed": PLUGIN})
    );
    assert_eq!(installed_version(), Some(json!("0.3.0")));
    worker.finish();
}

/// A grant request and a Settings history the worker read before the removal, as a
/// host prompt and an Access view left open in another process, cannot save once the
/// plugin was removed and the same bundle reinstalled; fresh reads save.
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn held_grant_reads_cannot_save_across_removal_and_same_hash_reinstall() {
    let first = bundle("0.2.0");
    assert_eq!(plugin_bundle::install(&first, &host()), Ok(PLUGIN.into()));
    grant();
    let mut worker = Worker::spawn();
    assert_eq!(
        worker.call(WorkerCommand::GrantRequest {
            name: "prompt".into()
        }),
        json!({"error": "", "epoch": 0, "keys": "unasked", "state": "allowed"}),
        "the prompt reads the stored answers and the first epoch"
    );
    assert_eq!(
        worker.call(WorkerCommand::GrantHistory {
            name: "settings".into()
        }),
        json!({"error": "", "epoch": 0, "entries": 1})
    );

    assert_eq!(app_begin_plugin_removal(PLUGIN.into()), "");
    let pending = worker.call(WorkerCommand::GrantRequest {
        name: "pending".into(),
    });
    assert!(
        pending["error"]
            .as_str()
            .is_some_and(|error| error.contains("removal is pending")),
        "no grant request while the removal is pending: {pending}"
    );
    let pending = worker.call(WorkerCommand::GrantSave {
        name: "prompt".into(),
    });
    assert!(
        pending["error"]
            .as_str()
            .is_some_and(|error| error.contains("removal is pending")),
        "the held prompt cannot save while the removal is pending: {pending}"
    );
    assert_eq!(plugin_bundle::plugin_remove(PLUGIN.into()), "");
    assert_eq!(phase(), None);
    assert_eq!(epoch(), Some(1));
    assert!(app_plugin_history(PLUGIN.into()).is_empty());

    assert_eq!(plugin_bundle::install(&first, &host()), Ok(PLUGIN.into()));
    assert_eq!(phase(), None, "the reinstall finds no removal journal");
    assert_eq!(installed_version(), Some(json!("0.2.0")));
    for name in ["prompt", "settings"] {
        let stale = worker.call(WorkerCommand::GrantSave { name: name.into() });
        assert!(
            stale["error"]
                .as_str()
                .is_some_and(|error| error.contains("removed since")),
            "the {name} read before the removal cannot save: {stale}"
        );
    }
    assert!(
        app_plugin_history(PLUGIN.into()).is_empty(),
        "no old answer is revived for the reinstalled library"
    );
    let grants = app_plugin_grants(plugin_hash());
    assert!(!grants.account && !grants.keys && !grants.state);

    assert_eq!(
        worker.call(WorkerCommand::GrantRequest {
            name: "fresh".into()
        }),
        json!({"error": "", "epoch": 1, "keys": "unasked", "state": "unasked"}),
        "a fresh prompt reads the rotated epoch and no answer"
    );
    assert_eq!(
        worker.call(WorkerCommand::GrantSave {
            name: "fresh".into()
        }),
        json!({"error": ""}),
        "a fresh prompt saves"
    );
    assert!(app_plugin_grants(plugin_hash()).keys);
    worker.finish();
}

#[test]
fn stale_preference_save_blocks_removal_phases_across_processes() {
    run_scenario("stale-preference");
}

#[test]
fn stale_snapshot_setter_keeps_private_data_phase_across_processes() {
    run_scenario("stale-snapshot");
}

#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
#[test]
fn old_session_stays_revoked_across_same_hash_reinstall_across_processes() {
    run_scenario("same-hash-reinstall");
}

#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
#[test]
fn paused_install_publishes_before_removal_across_processes() {
    run_scenario("paused-install");
}

#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
#[test]
fn pending_removal_refuses_install_from_another_process_across_processes() {
    run_scenario("pending-removal-install");
}

#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
#[test]
fn held_grant_reads_cannot_save_across_removal_and_reinstall_across_processes() {
    run_scenario("stale-grant");
}

/// A stored (uncompressed) ZIP of `entries`, as the bundle reader accepts it.
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
fn zip(entries: &[(&str, &[u8])]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut central = Vec::new();
    for (name, data) in entries {
        let crc = crc32fast::hash(data);
        let size = data.len() as u32;
        let offset = out.len() as u32;
        out.extend(0x0403_4b50_u32.to_le_bytes());
        for field in [20_u16, 0, 0, 0, 0x21] {
            out.extend(field.to_le_bytes());
        }
        for field in [crc, size, size] {
            out.extend(field.to_le_bytes());
        }
        for field in [name.len() as u16, 0] {
            out.extend(field.to_le_bytes());
        }
        out.extend(name.as_bytes());
        out.extend(*data);
        central.extend(0x0201_4b50_u32.to_le_bytes());
        for field in [(UNIX_HOST << 8) | 20, 20, 0, 0, 0, 0x21] {
            central.extend(field.to_le_bytes());
        }
        for field in [crc, size, size] {
            central.extend(field.to_le_bytes());
        }
        for field in [name.len() as u16, 0, 0, 0, 0] {
            central.extend(field.to_le_bytes());
        }
        central.extend((UNIX_REGULAR << 16).to_le_bytes());
        central.extend(offset.to_le_bytes());
        central.extend(name.as_bytes());
    }
    let directory = out.len() as u32;
    out.extend(&central);
    out.extend(0x0605_4b50_u32.to_le_bytes());
    let count = entries.len() as u16;
    for field in [0, 0, count, count] {
        out.extend(field.to_le_bytes());
    }
    out.extend((central.len() as u32).to_le_bytes());
    out.extend(directory.to_le_bytes());
    out.extend(0_u16.to_le_bytes());
    out
}

fn put(bytes: &mut [u8], offset: usize, value: u64, width: usize) {
    bytes[offset..offset + width].copy_from_slice(&value.to_le_bytes()[..width]);
}

/// The x86_64 ELF shared library of the admission tests, its Qt metadata `record`:
/// the two Qt exports, never meant to run.
fn elf(record: &[u8]) -> Vec<u8> {
    let names = b"\0.shstrtab\0.qtmetadata\0.dynsym\0.dynstr\0.text\0.dynamic\0";
    let strings = b"\0qt_plugin_instance\0qt_plugin_query_metadata_v2\0";
    let mut bytes = vec![0; 1360];
    bytes[..7].copy_from_slice(b"\x7fELF\x02\x01\x01");
    put(&mut bytes, 16, 3, 2);
    put(&mut bytes, 18, 62, 2);
    put(&mut bytes, 20, 1, 4);
    put(&mut bytes, 32, 64, 8);
    put(&mut bytes, 40, 232, 8);
    put(&mut bytes, 52, 64, 2);
    put(&mut bytes, 54, 56, 2);
    put(&mut bytes, 56, 3, 2);
    put(&mut bytes, 58, 64, 2);
    put(&mut bytes, 60, 7, 2);
    put(&mut bytes, 62, 1, 2);
    put(&mut bytes, 64, 1, 4);
    put(&mut bytes, 68, 5, 4);
    put(&mut bytes, 96, (1360 + record.len()) as u64, 8);
    put(&mut bytes, 104, (1360 + record.len()) as u64, 8);
    put(&mut bytes, 112, 4096, 8);
    put(&mut bytes, 120, 2, 4);
    put(&mut bytes, 124, 4, 4);
    put(&mut bytes, 128, 1248, 8);
    put(&mut bytes, 136, 1248, 8);
    put(&mut bytes, 152, 96, 8);
    put(&mut bytes, 160, 96, 8);
    put(&mut bytes, 168, 8, 8);
    for (index, name, kind, offset, size, flags) in [
        (1, 1, 3, 680, names.len(), 0),
        (2, 11, 1, 1360, record.len(), 2),
        (3, 23, 11, 1152, 72, 2),
        (4, 31, 3, 1024, strings.len(), 2),
        (5, 39, 1, 1344, 2, 6),
        (6, 45, 6, 1248, 96, 2),
    ] {
        let base = 232 + index * 64;
        put(&mut bytes, base, name, 4);
        put(&mut bytes, base + 4, kind, 4);
        put(&mut bytes, base + 8, flags, 8);
        if flags != 0 {
            put(&mut bytes, base + 16, offset, 8);
        }
        put(&mut bytes, base + 24, offset, 8);
        put(&mut bytes, base + 32, size as u64, 8);
        put(&mut bytes, base + 48, 1, 8);
    }
    put(&mut bytes, 232 + 3 * 64 + 40, 4, 4);
    put(&mut bytes, 232 + 3 * 64 + 56, 24, 8);
    put(&mut bytes, 232 + 6 * 64 + 40, 4, 4);
    put(&mut bytes, 232 + 6 * 64 + 56, 16, 8);
    bytes[680..680 + names.len()].copy_from_slice(names);
    bytes[1024..1024 + strings.len()].copy_from_slice(strings);
    for (index, value) in [1, 3, 1, 0, 2, 0].into_iter().enumerate() {
        put(&mut bytes, 960 + index * 4, value, 4);
    }
    for (index, name, address) in [(1, 1, 1344), (2, 20, 1345)] {
        let base = 1152 + index * 24;
        put(&mut bytes, base, name, 4);
        bytes[base + 4] = 0x12;
        put(&mut bytes, base + 6, 5, 2);
        put(&mut bytes, base + 8, address, 8);
        put(&mut bytes, base + 16, 1, 8);
    }
    for (index, tag, value) in [
        (0, 6, 1152),
        (1, 11, 24),
        (2, 5, 1024),
        (3, 10, strings.len() as u64),
        (4, 4, 960),
    ] {
        put(&mut bytes, 1248 + index * 16, tag, 8);
        put(&mut bytes, 1256 + index * 16, value, 8);
    }
    bytes[1344..1346].copy_from_slice(&[0xc3, 0xc3]);
    bytes.extend(record);
    bytes
}
