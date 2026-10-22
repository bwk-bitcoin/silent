use std::{
    env, fs,
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    thread,
    time::{Duration, Instant},
};

use temp_dir::TempDir;

use crate::{
    config::set_datadir,
    keys::crypto::Password,
    state_crypt::{
        create_with_device, create_with_password, lock, seal, seal_with,
        setup::{SetupFiles, SetupStage, TEST_HOOK, TRANSACTION},
        state_unlock_info, unlock_with_device, unlock_with_password, xpub_key, CHECK_FILE,
        CHECK_TEXT, KEY_FILE,
    },
};

const XPUB: &str = "tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXTexf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa";
const PASSWORD: &str = "correct horse setup";
const LOSER: &str = "second instance setup";
const CHILD: &str = "state_crypt::setup::tests::setup_child";

fn wait_for(path: &Path) {
    let started = Instant::now();
    while !path.exists() {
        assert!(
            started.elapsed() < Duration::from_secs(20),
            "timed out waiting for {}",
            path.display()
        );
        thread::sleep(Duration::from_millis(2));
    }
}

struct Process(Child);

impl Process {
    fn spawn(case: &str, data: &Path, control: &Path, stage: &str, mode: &str) -> Self {
        Self(
            Command::new(env::current_exe().expect("test executable"))
                .args(["--exact", CHILD, "--nocapture"])
                .env("SILENT_SETUP_CASE", case)
                .env("SILENT_SETUP_DATA", data)
                .env("SILENT_SETUP_CONTROL", control)
                .env("SILENT_SETUP_STAGE", stage)
                .env("SILENT_SETUP_MODE", mode)
                .env("HOME", data)
                .stdout(Stdio::null())
                .spawn()
                .expect("spawn isolated setup process"),
        )
    }

    fn success(&mut self) {
        let start = Instant::now();
        loop {
            if let Some(status) = self.0.try_wait().expect("poll setup child") {
                assert!(status.success(), "setup child failed: {status}");
                break;
            }
            assert!(
                start.elapsed() < Duration::from_secs(30),
                "setup child timed out"
            );
            thread::sleep(Duration::from_millis(2));
        }
    }

    fn kill(&mut self) {
        self.0
            .kill()
            .expect("kill setup without running destructors");
        assert!(
            !self.0.wait().expect("wait for killed setup").success(),
            "setup was interrupted"
        );
    }
}

impl Drop for Process {
    fn drop(&mut self) {
        if self.0.try_wait().expect("poll child on cleanup").is_none() {
            self.0.kill().expect("clean up setup child");
            self.0.wait().expect("reap setup child");
        }
    }
}

fn password(value: &str) -> Password {
    Password::new(value.into())
}

fn create(mode: &str) -> Result<(), String> {
    match mode {
        "password" => create_with_password(password(PASSWORD)),
        "device" => create_with_device(42, XPUB),
        _ => panic!("unknown setup mode {mode}"),
    }
}

fn verify(mode: &str) {
    let info = state_unlock_info();
    assert!(
        info.error.is_empty() && info.exists,
        "committed state is readable: {}",
        info.error
    );
    match mode {
        "password" => {
            assert!(
                !info.device && info.path.is_empty(),
                "password classification comes from check"
            );
            unlock_with_password(password(PASSWORD)).expect("winning password unlocks saved key");
        }
        "device" => {
            assert!(info.device, "device classification comes from check");
            assert_eq!(
                info.path, "m/138'/1936225652'/42'",
                "legacy device metadata retained"
            );
            unlock_with_device(XPUB).expect("software fixture unlocks saved device check");
        }
        _ => panic!("unknown unlock mode {mode}"),
    }
    let bytes = seal("still unlocked").expect("verified key can encrypt state");
    assert_eq!(
        crate::state_crypt::open(&bytes).expect("reopen state"),
        "still unlocked"
    );
}

fn stage(value: &str) -> SetupStage {
    match value {
        "journal-partial" => SetupStage::JournalPartial,
        "journal-synced" => SetupStage::JournalSynced,
        "journal" => SetupStage::Journal,
        "key" => SetupStage::Key,
        "check-synced" => SetupStage::CheckSynced,
        "check" => SetupStage::Check,
        "finished" => SetupStage::Finished,
        _ => panic!("unknown checkpoint {value}"),
    }
}

fn pause_at(checkpoint: SetupStage, control: PathBuf) {
    TEST_HOOK.with(|hook| {
        *hook.borrow_mut() = Some(Box::new(move |seen| {
            if seen == checkpoint {
                fs::write(control.join("paused"), []).expect("signal publication checkpoint");
                wait_for(&control.join("continue"));
            }
            Ok(())
        }));
    });
}

fn failure_after_key(data: &Path) {
    let root = data.to_path_buf();
    TEST_HOOK.with(|hook| {
        *hook.borrow_mut() = Some(Box::new(move |seen| {
            if seen == SetupStage::CheckSynced {
                assert!(
                    root.join(KEY_FILE).is_file(),
                    "password key was actually published"
                );
                assert!(
                    !root.join(CHECK_FILE).exists(),
                    "check publication has not committed"
                );
                return Err("injected check publication failure".into());
            }
            Ok(())
        }));
    });
    let error = create_with_password(password(PASSWORD))
        .expect_err("check write fails after key publication");
    assert_eq!(error, "injected check publication failure");
    TEST_HOOK.with(|hook| hook.borrow_mut().take());
    assert!(
        !data.join(KEY_FILE).exists(),
        "failed attempt removes only its own published password key"
    );
    assert!(
        !data.join(CHECK_FILE).exists(),
        "failed attempt has no committed check"
    );
    assert!(
        !data.join(TRANSACTION).exists(),
        "rollback retired its transaction"
    );
    let info = state_unlock_info();
    assert!(
        !info.exists && info.error.is_empty(),
        "failed setup remains cleanly uninitialized: {}",
        info.error
    );
    create_with_device(42, XPUB).expect("Back then Device succeeds after failed password setup");
    assert!(
        !data.join(KEY_FILE).exists(),
        "device success leaves no unrelated password key"
    );
    lock();
    verify("device");
}

fn refuse_existing(data: &Path, mode: &str) {
    create(mode).expect("original state created");
    let key = fs::read(data.join(KEY_FILE)).ok();
    let check = fs::read(data.join(CHECK_FILE)).expect("original check");
    let info = state_unlock_info();
    assert!(
        info.exists && info.error.is_empty(),
        "original state readable"
    );
    assert!(
        create_with_password(password(LOSER)).is_err(),
        "stale password setup refused"
    );
    assert!(
        create_with_device(87, XPUB).is_err(),
        "stale device setup refused"
    );
    assert_eq!(
        fs::read(data.join(KEY_FILE)).ok(),
        key,
        "prior key bytes never changed"
    );
    assert_eq!(
        fs::read(data.join(CHECK_FILE)).expect("check preserved"),
        check,
        "prior check never changed"
    );
    lock();
    verify(mode);
}

fn orphan(data: &Path) {
    let bytes = b"unowned previous envelope";
    fs::write(data.join(KEY_FILE), bytes).expect("unowned orphan fixture");
    let info = state_unlock_info();
    assert!(
        !info.exists && info.error.contains("recovery required"),
        "orphan cannot claim complete state: {}",
        info.error
    );
    assert!(
        create_with_device(42, XPUB).is_err(),
        "device cannot succeed over unrelated password key"
    );
    assert!(
        create_with_password(password(PASSWORD)).is_err(),
        "password cannot overwrite unowned orphan"
    );
    assert_eq!(
        fs::read(data.join(KEY_FILE)).expect("unowned key retained"),
        bytes,
        "unowned key never deleted"
    );
    assert!(
        !data.join(CHECK_FILE).exists(),
        "no misleading successful device check"
    );
}

fn conflicting_transaction(data: &Path) {
    create("password").expect("valid prior password state");
    let key = fs::read(data.join(KEY_FILE)).expect("prior password envelope");
    let check = fs::read(data.join(CHECK_FILE)).expect("prior unlock check");
    let journal = SetupFiles::Password {
        key: key.clone(),
        check: seal_with(xpub_key(XPUB).expect("fixture xpub"), CHECK_TEXT, None)
            .expect("different encrypted check"),
    };
    fs::write(
        data.join(TRANSACTION),
        serde_json::to_vec(&journal).expect("conflicting transaction fixture"),
    )
    .expect("install inspected conflict fixture");
    let info = state_unlock_info();
    assert!(
        info.error.contains("conflicts"),
        "recovery refuses a journal that does not own the committed check: {}",
        info.error
    );
    assert_eq!(
        fs::read(data.join(KEY_FILE)).expect("prior key retained"),
        key,
        "recovery never deletes the prior key even when the journal copies its bytes"
    );
    assert_eq!(
        fs::read(data.join(CHECK_FILE)).expect("prior check retained"),
        check,
        "recovery never replaces the prior check"
    );
    fs::rename(data.join(TRANSACTION), data.join("conflicting-setup.tmp"))
        .expect("move the inspected fixture aside without erasing it");
    lock();
    verify("password");
}

#[test]
fn setup_child() {
    let Ok(case) = env::var("SILENT_SETUP_CASE") else {
        return;
    };
    let data = PathBuf::from(env::var_os("SILENT_SETUP_DATA").expect("isolated state directory"));
    let control =
        PathBuf::from(env::var_os("SILENT_SETUP_CONTROL").expect("coordination directory"));
    let mode = env::var("SILENT_SETUP_MODE").expect("unlock mode");
    set_datadir(data.clone());
    fs::create_dir_all(&data).expect("isolated data directory");
    match case.as_str() {
        "winner" | "loser" => {
            let info = state_unlock_info();
            assert!(
                !info.exists && info.error.is_empty(),
                "both instances initially opened Setup"
            );
            let label = case.as_str();
            fs::write(control.join(format!("{label}-ready")), []).expect("signal stale setup open");
            wait_for(&control.join(format!("{label}-go")));
            if case == "winner" {
                pause_at(SetupStage::Key, control.clone());
                create("password").expect("winner creates original password state");
                verify("password");
            } else {
                let error = create_with_password(password(LOSER))
                    .expect_err("concurrent loser refused by lock");
                assert!(
                    error.contains("Busy"),
                    "whole setup is exclusively held: {error}"
                );
                fs::write(control.join("loser-refused"), []).expect("signal concurrent refusal");
                wait_for(&control.join("retry"));
                let error = create_with_password(password(LOSER))
                    .expect_err("stale setup refused after winner commits");
                assert_eq!(error, "the app state already has an unlock key");
                assert!(
                    unlock_with_password(password(LOSER)).is_err(),
                    "loser credential never replaced winner"
                );
                verify("password");
            }
        }
        "crash" => {
            pause_at(
                stage(&env::var("SILENT_SETUP_STAGE").expect("crash phase")),
                control,
            );
            create(&mode).expect("interrupted before returning success");
            panic!("parent must terminate the paused setup");
        }
        "recover-empty" => {
            let info = state_unlock_info();
            assert!(
                !info.exists && info.error.is_empty(),
                "owned interrupted setup rolled back: {}",
                info.error
            );
            assert!(
                !data.join(KEY_FILE).exists(),
                "no orphan key after recovery"
            );
            assert!(!data.join(TRANSACTION).exists(), "recovery retired journal");
            create("device").expect("new Device setup succeeds after recovered crash");
            verify("device");
        }
        "verify" => verify(&mode),
        "failure" => failure_after_key(&data),
        "existing" => refuse_existing(&data, &mode),
        "orphan" => orphan(&data),
        "conflict" => conflicting_transaction(&data),
        "inconsistent" => {
            create("device").expect("committed legacy device state");
            fs::write(data.join(KEY_FILE), b"unrelated key")
                .expect("legacy inconsistent pair fixture");
            let info = state_unlock_info();
            assert!(
                !info.error.is_empty() && !info.device,
                "inconsistent pair is an explicit error, not a password state"
            );
            assert!(
                create_with_password(password(LOSER)).is_err(),
                "existing inconsistent state cannot be overwritten"
            );
            assert_eq!(
                fs::read(data.join(KEY_FILE)).expect("key retained"),
                b"unrelated key"
            );
        }
        _ => panic!("unknown child case {case}"),
    }
}

#[test]
fn setup_concurrent_processes_preserve_the_winning_password() {
    let data = TempDir::new().expect("fresh shared state");
    let control = TempDir::new().expect("setup coordination");
    let mut winner = Process::spawn("winner", data.path(), control.path(), "", "password");
    wait_for(&control.path().join("winner-ready"));
    let mut loser = Process::spawn("loser", data.path(), control.path(), "", "password");
    wait_for(&control.path().join("loser-ready"));
    fs::write(control.path().join("winner-go"), []).expect("start first setup");
    wait_for(&control.path().join("paused"));
    let key = fs::read(data.path().join(KEY_FILE)).expect("winner's actual published key");
    let journal = fs::read(data.path().join(TRANSACTION)).expect("durable ownership journal");
    assert!(
        !String::from_utf8_lossy(&journal).contains(PASSWORD),
        "journal holds no clear password"
    );
    assert!(
        !String::from_utf8_lossy(&journal).contains(XPUB),
        "journal holds no clear device xpub"
    );
    assert!(
        !data.path().join(CHECK_FILE).exists(),
        "first setup is paused before check publication"
    );
    fs::write(control.path().join("loser-go"), []).expect("attempt overlapping setup");
    wait_for(&control.path().join("loser-refused"));
    assert_eq!(
        fs::read(data.path().join(KEY_FILE)).expect("key still present"),
        key,
        "concurrent loser changed no bytes"
    );
    fs::write(control.path().join("continue"), []).expect("allow winner to commit");
    winner.success();
    let check = fs::read(data.path().join(CHECK_FILE)).expect("winner's committed check");
    fs::write(control.path().join("retry"), []).expect("exercise stale modal after commit");
    loser.success();
    assert_eq!(
        fs::read(data.path().join(KEY_FILE)).expect("original key"),
        key,
        "stale retry preserved key bytes"
    );
    assert_eq!(
        fs::read(data.path().join(CHECK_FILE)).expect("original check"),
        check,
        "stale retry preserved check bytes"
    );
    Process::spawn("verify", data.path(), control.path(), "", "password").success();
}

#[test]
fn setup_failure_after_key_allows_device_setup_and_startup() {
    let data = TempDir::new().expect("failed setup state");
    let control = TempDir::new().expect("failure coordination");
    Process::spawn("failure", data.path(), control.path(), "", "password").success();
    Process::spawn("verify", data.path(), control.path(), "", "device").success();
}

#[test]
fn setup_crash_recovery_respects_the_check_commit_point() {
    for (mode, phases) in [
        (
            "password",
            vec![
                "journal-partial",
                "journal-synced",
                "journal",
                "key",
                "check-synced",
                "check",
                "finished",
            ],
        ),
        (
            "device",
            vec![
                "journal-partial",
                "journal-synced",
                "journal",
                "check-synced",
                "check",
                "finished",
            ],
        ),
    ] {
        for phase in phases {
            let data = TempDir::new().expect("crash state");
            let control = TempDir::new().expect("crash coordination");
            let mut child = Process::spawn("crash", data.path(), control.path(), phase, mode);
            wait_for(&control.path().join("paused"));
            let key = fs::read(data.path().join(KEY_FILE)).ok();
            let check = fs::read(data.path().join(CHECK_FILE)).ok();
            child.kill();
            let committed = matches!(phase, "check" | "finished");
            let case = if committed { "verify" } else { "recover-empty" };
            Process::spawn(case, data.path(), control.path(), "", mode).success();
            if committed {
                assert_eq!(
                    fs::read(data.path().join(KEY_FILE)).ok(),
                    key,
                    "{mode} crash at {phase} preserves committed key"
                );
                assert_eq!(
                    fs::read(data.path().join(CHECK_FILE)).ok(),
                    check,
                    "{mode} crash at {phase} preserves committed check"
                );
            } else {
                Process::spawn("verify", data.path(), control.path(), "", "device").success();
            }
        }
    }
}

#[test]
fn setup_preserves_existing_keys_and_refuses_unowned_incomplete_state() {
    for (case, mode) in [
        ("existing", "password"),
        ("existing", "device"),
        ("orphan", "password"),
        ("inconsistent", "device"),
        ("conflict", "password"),
    ] {
        let data = TempDir::new().expect("preservation state");
        let control = TempDir::new().expect("preservation coordination");
        Process::spawn(case, data.path(), control.path(), "", mode).success();
    }
}
