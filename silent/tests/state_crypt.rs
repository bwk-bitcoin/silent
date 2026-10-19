//! The app state's unlock key: created with a password or a device xpub, required to
//! read or write the state, and a file left in clear is found (ws-plaude/silent#37).

use std::fs;

use silent::keys::crypto::Password;
use silent::set_datadir;
use silent::state_crypt::{self, UnlockKind};

const XPUB: &str = "tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtmnUJSonudSK8HcXTexf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa";
const OTHER_XPUB: &str = "tpubDDzMtUDKwjvTehAmAfwYiBGzUFDcF8AyEaJf78JhQam5knvsGTgqF7VvhuuDZLYityND66FG88sS3q28Yi5Ysumvx5DfMgdm6nc6zntvpHM";

// One test: the data directory and the unlock key are process wide.
#[test]
fn the_state_opens_with_its_unlock_key_only() {
    let tmp = temp_dir::TempDir::new().expect("create temp dir");
    let base = tmp.path().to_path_buf();
    set_datadir(base.clone());
    let file = base.join("account").join("config.json");

    // Nothing created yet: locked, nothing can be written.
    assert_eq!(state_crypt::unlock_info().unwrap(), None);
    assert!(!state_crypt::is_unlocked());
    assert!(state_crypt::write_state(&file, "{}").is_err(), "locked");

    // A password key.
    state_crypt::create_with_password(Password::new("correct horse".into())).unwrap();
    let info = state_crypt::unlock_info().unwrap().expect("unlock.json");
    assert_eq!(info.kind, UnlockKind::Password);
    state_crypt::write_state(&file, "{\"name\":\"a\"}").unwrap();
    let sealed = fs::read(&file).unwrap();
    assert!(state_crypt::is_sealed(&sealed));
    assert!(!String::from_utf8_lossy(&sealed).contains("\"name\""));

    state_crypt::lock();
    assert!(state_crypt::read_state(&file).is_err(), "locked again");
    assert!(
        state_crypt::unlock_with_password(Password::new("wrong".into())).is_err(),
        "a wrong password does not unlock"
    );
    state_crypt::unlock_with_password(Password::new("correct horse".into())).unwrap();
    assert_eq!(
        state_crypt::read_state(&file).unwrap().as_deref(),
        Some("{\"name\":\"a\"}")
    );

    // A second key is refused: the unlock key is fixed when the state is created.
    assert!(state_crypt::create_with_device("73c5da0a", "Ledger", 5, "regtest", XPUB).is_err());

    // A device key, on a fresh state.
    state_crypt::lock();
    for name in [
        state_crypt::UNLOCK_FILE,
        state_crypt::KEY_FILE,
        "unlock.check",
    ] {
        let _ = fs::remove_file(base.join(name));
    }
    let index = state_crypt::draw_index().unwrap();
    state_crypt::create_with_device("73c5da0a", "Ledger", index, "regtest", XPUB).unwrap();
    let info = state_crypt::unlock_info().unwrap().expect("unlock.json");
    assert_eq!(
        (
            info.kind,
            info.fingerprint.as_str(),
            info.alias.as_str(),
            info.index,
            info.network.as_str()
        ),
        (UnlockKind::Device, "73c5da0a", "Ledger", index, "regtest")
    );
    let clear = fs::read_to_string(base.join(state_crypt::UNLOCK_FILE)).unwrap();
    assert!(!clear.contains(XPUB), "the device xpub is never written");
    assert!(
        state_crypt::read_state(&file).is_err(),
        "a file sealed to the password key does not open with the device key"
    );
    state_crypt::write_state(&file, "{\"name\":\"b\"}").unwrap();

    state_crypt::lock();
    assert!(
        state_crypt::unlock_with_device(OTHER_XPUB).is_err(),
        "another xpub"
    );
    state_crypt::unlock_with_device(XPUB).unwrap();
    assert_eq!(
        state_crypt::read_state(&file).unwrap().as_deref(),
        Some("{\"name\":\"b\"}")
    );

    // A plugin's own state: bytes, sealed, by plugin id.
    assert_eq!(state_crypt::read_plugin_state("example").unwrap(), None);
    state_crypt::write_plugin_state("example", b"\x00plugin bytes").unwrap();
    assert_eq!(
        state_crypt::read_plugin_state("example")
            .unwrap()
            .as_deref(),
        Some(&b"\x00plugin bytes"[..])
    );
    let sealed = fs::read(base.join("plugin-state").join("example.json")).unwrap();
    assert!(state_crypt::is_sealed(&sealed));
    assert!(state_crypt::write_plugin_state("../escape", b"x").is_err());

    // A state file in clear is found, and refused: bwk's stores too. Its bookkeeping
    // files stay in clear.
    fs::write(base.join("account").join("version"), "1").unwrap();
    fs::write(base.join("account").join(".lock"), "").unwrap();
    assert_eq!(state_crypt::clear_state_file(), None);
    let bwk_file = base.join("account").join("account.json");
    fs::write(&bwk_file, "{}").unwrap();
    assert_eq!(state_crypt::clear_state_file(), Some(bwk_file.clone()));
    fs::remove_file(&bwk_file).unwrap();
    let clear_file = base.join("old").join("config.json");
    fs::create_dir_all(clear_file.parent().unwrap()).unwrap();
    fs::write(&clear_file, "{\"name\":\"c\"}").unwrap();
    assert_eq!(state_crypt::clear_state_file(), Some(clear_file.clone()));
    assert!(state_crypt::read_state(&clear_file).is_err());
}
