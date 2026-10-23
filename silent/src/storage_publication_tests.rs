use std::{
    env, fs,
    process::{Command, Stdio},
    sync::{mpsc, Arc, Mutex},
    thread,
    time::{Duration, Instant},
};

use base64::{prelude::BASE64_STANDARD, Engine};
use serde_json::{json, Value};
use temp_dir::TempDir;

use crate::{
    app_state::{app_plugin_access_request, app_plugin_grants, app_set_plugin_access},
    config::set_datadir,
    ffi::{PluginAccess, PluginGrant},
    state_crypt::{self, on_durable_write_at, DurableWriteStage},
    storage::{
        storage_request, storage_request_cancel, storage_session_generation, storage_session_open,
        storage_session_revoke, DOCUMENT,
    },
};

const PLUGIN: &str = "publication-test";
const HASH: &str = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/// Asks and saves the Saved Data grant: the error, empty once saved.
fn grant(state: PluginGrant) -> String {
    let request = app_plugin_access_request(PLUGIN.into(), HASH.into());
    if !request.is_ok {
        return request.error;
    }
    app_set_plugin_access(
        PluginAccess {
            state,
            ..request.access
        },
        request.storage_epoch,
    )
}

fn send(session: u64, mut message: Value) -> Value {
    message["id"] = json!("publication");
    serde_json::from_str(&storage_request(session, message.to_string()))
        .expect("serialized publication response")
}

fn open(session: u64) -> String {
    let opened = send(session, json!({"op":"open", "storage":"private"}));
    assert_eq!(opened["error"], "", "private storage opens: {opened}");
    opened["storage"].as_str().expect("storage handle").into()
}

struct Fixture {
    dir: TempDir,
    session: u64,
    handle: String,
}

impl Fixture {
    fn new() -> Self {
        let dir = TempDir::new().expect("isolated publication state");
        set_datadir(dir.path().to_path_buf());
        state_crypt::unlock_for_tests();
        assert_eq!(grant(PluginGrant::Allowed), "", "initial grant saved");
        let session = storage_session_open(PLUGIN.into(), HASH.into(), true);
        let handle = open(session);
        let fixture = Self {
            dir,
            session,
            handle,
        };
        assert_eq!(
            fixture.write(b"old")["error"],
            "",
            "initial whole file commits"
        );
        fixture
    }

    fn write(&self, bytes: &[u8]) -> Value {
        send(
            self.session,
            json!({"op":"write", "storage":self.handle,
            "path":"file", "mode":"upsert", "data":BASE64_STANDARD.encode(bytes)}),
        )
    }

    fn tree(&self) -> Vec<u8> {
        fs::read(self.dir.path().join(PLUGIN).join(DOCUMENT)).expect("authoritative encrypted tree")
    }

    fn stored(&self) -> Value {
        let witness = storage_session_open(PLUGIN.into(), String::new(), false);
        let handle = open(witness);
        let result = send(
            witness,
            json!({"op":"read", "storage":handle, "path":"file"}),
        );
        storage_session_revoke(witness);
        result
    }
}

fn revocation(stage: DurableWriteStage, fail_save: bool) {
    let f = Fixture::new();
    let app_path = f.dir.path().join("app.bip138");
    let app_before = fs::read(&app_path).expect("original persisted grant");
    let tree_before = f.tree();
    let generation = storage_session_generation(f.session);
    if fail_save {
        fs::create_dir(f.dir.path().join("app.tmp")).expect("real grant-write failure fixture");
    }

    let answer = match stage {
        DurableWriteStage::Authorized | DurableWriteStage::TempSynced => {
            let (go_tx, go_rx) = mpsc::channel();
            let (started_tx, started_rx) = mpsc::channel();
            let (done_tx, done_rx) = mpsc::channel();
            let done_rx = Arc::new(Mutex::new(done_rx));
            let checkpoint_done = Arc::clone(&done_rx);
            let revoker = thread::spawn(move || {
                go_rx
                    .recv_timeout(Duration::from_secs(5))
                    .expect("revocation requested");
                started_tx
                    .send(())
                    .expect("revocation thread attempting update");
                let saved = grant(PluginGrant::Denied);
                assert_eq!(
                    saved.is_empty(),
                    !fail_save,
                    "the revocation reports its save: {saved}"
                );
                done_tx.send(()).expect("grant update returned");
            });
            let paused_tree = f.dir.path().join(PLUGIN).join(DOCUMENT);
            let paused_before = tree_before.clone();
            on_durable_write_at(stage, move || {
                go_tx.send(()).expect("revoke after final authorization");
                started_rx
                    .recv_timeout(Duration::from_secs(5))
                    .expect("revoker started");
                assert!(
                    matches!(
                        checkpoint_done
                            .lock()
                            .expect("completion receiver")
                            .recv_timeout(Duration::from_millis(100)),
                        Err(mpsc::RecvTimeoutError::Timeout)
                    ),
                    "grant update cannot split the authorized storage transaction"
                );
                assert!(
                    app_plugin_grants(HASH.into()).state,
                    "the persisted grant is still allowed during the authorized commit"
                );
                assert_eq!(
                    fs::read(paused_tree).expect("tree before rename"),
                    paused_before,
                    "the writer has not published while revocation is waiting"
                );
            });
            let answer = f.write(b"new");
            done_rx
                .lock()
                .expect("completion receiver")
                .recv_timeout(Duration::from_secs(5))
                .expect("grant update completes after authorized rename");
            revoker.join().expect("revocation thread completed");
            assert!(
                answer["error"] == "" || answer["error"] == "denied",
                "committed write may have its reply fenced by revocation: {answer}"
            );
            answer
        }
        stage => panic!("unexpected grant checkpoint {stage:?}"),
    };

    if fail_save {
        assert_eq!(
            fs::read(app_path).expect("grant after failed write"),
            app_before,
            "failed grant persistence retains the exact old encrypted state"
        );
        assert!(
            app_plugin_grants(HASH.into()).state,
            "a failed revocation retains the old allowed grant"
        );
        assert_eq!(
            storage_session_generation(f.session),
            generation,
            "failed persistence never invalidates an authorized generation"
        );
        assert_eq!(
            answer["error"], "",
            "a failed revocation does not falsely fence the write"
        );
    } else {
        assert!(
            !app_plugin_grants(HASH.into()).state,
            "the completed revocation is persisted"
        );
        assert_ne!(
            storage_session_generation(f.session),
            generation,
            "successful revocation invalidates the old generation"
        );
        assert!(
            answer["error"] == "" || answer["error"] == "denied",
            "the serialized write commits before revocation; its reply may be fenced: {answer}"
        );
    }
    let expected = b"new";
    let stored = f.stored();
    assert_eq!(
        stored["error"], "",
        "the authoritative tree remains readable: {stored}"
    );
    assert_eq!(
        stored["data"],
        BASE64_STANDARD.encode(expected),
        "the tree reflects the actual commit order, not just a suppressed reply"
    );
    let before_next = f.tree();
    assert_eq!(
        f.write(b"later")["error"],
        if fail_save { "" } else { "denied" },
        "subsequent writes use only the grant that was actually saved"
    );
    if !fail_save {
        assert_eq!(
            f.tree(),
            before_next,
            "no mutation publishes after completed revocation"
        );
    }
    storage_session_revoke(f.session);
}

fn lifecycle(revoke: bool) {
    let f = Fixture::new();
    let session = f.session;
    on_durable_write_at(DurableWriteStage::Authorized, move || {
        if revoke {
            storage_session_revoke(session);
        } else {
            storage_request_cancel(session, "publication".into());
        }
    });
    let answer = f.write(b"authorized");
    assert_eq!(
        answer["error"],
        if revoke { "revoked" } else { "cancelled" },
        "unload/cancellation fences the response without draining an authorized commit"
    );
    let stored = f.stored();
    assert_eq!(stored["error"], "", "the authorized tree remains readable");
    assert_eq!(
        stored["data"],
        BASE64_STANDARD.encode(b"authorized"),
        "the already-authorized commit may finish whole"
    );
    assert_eq!(
        f.write(b"later")["error"],
        if revoke { "revoked" } else { "" },
        "unload is permanent; request cancellation does not revoke the session"
    );
    storage_session_revoke(f.session);
}

fn unreadable_grants() {
    let f = Fixture::new();
    let path = f.dir.path().join("app.bip138");
    let original = fs::read(&path).expect("original granted state");
    let generation = storage_session_generation(f.session);
    fs::write(&path, b"unreadable grant state").expect("actual unreadable app state");
    let refused = grant(PluginGrant::Denied);
    assert!(
        refused.contains("not encrypted"),
        "an unreadable state refuses the grant request: {refused}"
    );
    assert_eq!(
        fs::read(&path).expect("refused grant state"),
        b"unreadable grant state",
        "a failed grant load never overwrites the state with defaults"
    );
    assert_eq!(
        storage_session_generation(f.session),
        generation,
        "failed grant loading never reports a successful invalidation"
    );
    fs::write(&path, original).expect("restore the original state");
    assert!(
        app_plugin_grants(HASH.into()).state,
        "the original persisted grant is still authoritative"
    );
    assert_eq!(
        f.write(b"restored")["error"],
        "",
        "failure did not falsely invalidate the session"
    );
    storage_session_revoke(f.session);
}

#[test]
fn publication_process() {
    let Ok(case) = env::var("SILENT_GRANT_PUBLICATION_CASE") else {
        return;
    };
    match case.as_str() {
        "grant-after-auth" => revocation(DurableWriteStage::Authorized, false),
        "grant-before-auth" => revocation(DurableWriteStage::TempSynced, false),
        "failed-grant-after-auth" => revocation(DurableWriteStage::Authorized, true),
        "failed-grant-before-auth" => revocation(DurableWriteStage::TempSynced, true),
        "unload-after-auth" => lifecycle(true),
        "cancel-after-auth" => lifecycle(false),
        "unreadable-grants" => unreadable_grants(),
        case => panic!("unexpected publication case {case}"),
    }
}

#[test]
fn grant_publication_order_save_failures_and_nonblocking_lifecycle() {
    for case in [
        "grant-after-auth",
        "grant-before-auth",
        "failed-grant-after-auth",
        "failed-grant-before-auth",
        "unload-after-auth",
        "cancel-after-auth",
        "unreadable-grants",
    ] {
        let mut child = Command::new(env::current_exe().expect("current test executable"))
            .args([
                "--exact",
                "storage::publication_tests::publication_process",
                "--nocapture",
            ])
            .env("SILENT_GRANT_PUBLICATION_CASE", case)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("run isolated real grant publication test");
        let started = Instant::now();
        while child
            .try_wait()
            .expect("publication process status")
            .is_none()
        {
            if started.elapsed() > Duration::from_secs(10) {
                child.kill().expect("stop a blocked publication test");
                let output = child.wait_with_output().expect("blocked process output");
                panic!(
                    "publication case {case} blocked:\n{}\n{}",
                    String::from_utf8_lossy(&output.stdout),
                    String::from_utf8_lossy(&output.stderr)
                );
            }
            thread::sleep(Duration::from_millis(5));
        }
        let output = child
            .wait_with_output()
            .expect("publication process output");
        assert!(
            output.status.success(),
            "publication case {case} failed:\n{}\n{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
    }
}
