//! Run with the built Qt host and the real external example plugin.

use std::{env, path::PathBuf, process::Command};

use base64::{engine::general_purpose::STANDARD, Engine};
use silent::{keys::crypto::Password, set_datadir, state_crypt};
use temp_dir::TempDir;

#[test]
#[ignore = "requires SILENT_HOST_SMOKE and SILENT_EXAMPLE_PLUGIN built against current headers"]
fn example_state_migrates_and_saves_through_the_public_host() {
    let host = PathBuf::from(env::var_os("SILENT_HOST_SMOKE").expect("built host_smoke path"));
    let plugin = PathBuf::from(env::var_os("SILENT_EXAMPLE_PLUGIN").expect("example bundle path"));
    let home = TempDir::new().expect("isolated example consumer HOME");
    let data = home.path().join(".silent");
    set_datadir(data.clone());
    state_crypt::create_with_password(Password::new("host smoke".into()))
        .expect("create the same encrypted state the Qt fixture unlocks");
    let legacy = state_crypt::legacy_plugin_state_path(&data, "example")
        .expect("host-side legacy plugin location");
    let payload = br#"{"example.legacy":{"name":"Legacy account","settingEnabled":true}}"#;
    state_crypt::write_state(&legacy, &STANDARD.encode(payload))
        .expect("seed the exact former ExampleState payload without an old public API");
    let output = Command::new(&host)
        .arg("--storage-example")
        .arg(&plugin)
        .env("HOME", home.path())
        .env("QT_QPA_PLATFORM", "offscreen")
        .output()
        .expect("execute the built Qt public-contract fixture");
    assert!(
        output.status.success(),
        "real example migration/save fixture failed:\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(
        String::from_utf8_lossy(&output.stdout).contains("Example storage smoke: OK"),
        "the real external consumer fixture ran to completion"
    );
    assert!(
        !legacy.exists(),
        "plugin removal deletes the actual legacy blob migration source"
    );
    let restarted = Command::new(&host)
        .arg("--storage-example-reinstall")
        .arg(&plugin)
        .env("HOME", home.path())
        .env("QT_QPA_PLATFORM", "offscreen")
        .output()
        .expect("restart the actual host and reinstall the bundle");
    assert!(
        restarted.status.success(),
        "restarted external consumer failed:\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&restarted.stdout),
        String::from_utf8_lossy(&restarted.stderr)
    );
    assert!(
        String::from_utf8_lossy(&restarted.stdout).contains("Example reinstall smoke: OK"),
        "the real reinstalled consumer completed with empty private data"
    );
    state_crypt::lock();
}
