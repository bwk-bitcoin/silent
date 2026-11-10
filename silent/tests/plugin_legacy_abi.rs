//! Run with the built Qt host and the ABI fixtures built beside it.

use std::{env, fs, path::PathBuf, process::Command};

use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::json;
use silent::{keys::crypto::Password, plugin_bundle, set_datadir, state_crypt};
use temp_dir::TempDir;

#[test]
#[ignore = "requires SILENT_HOST_SMOKE built with its ABI fixtures"]
fn legacy_bundles_of_incompatible_abis_never_enable() {
    let host = PathBuf::from(env::var_os("SILENT_HOST_SMOKE").expect("built host_smoke path"));
    let fixtures = host.parent().expect("host_smoke build directory");
    let home = TempDir::new().expect("isolated legacy ABI HOME");
    let data = home.path().join(".silent");
    set_datadir(data.clone());
    state_crypt::create_with_password(Password::new("host smoke".into()))
        .expect("create the same encrypted state the Qt fixture unlocks");
    let versions = [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];
    for version in versions {
        let id = format!("abi{version}");
        let library = fs::read(fixtures.join(format!("libplugin_abi{version}_fixture.so")))
            .expect("compiled ABI fixture");
        // The bundle an earlier Silent sealed from a folder import, before admission.
        let bundle = json!({
            "version": 1,
            "sidecar": {
                "id": id,
                "name": "ABI fixture",
                "version": "1.0.0",
                "description": "Legacy ABI fixture",
            },
            "library_name": format!("lib{id}.so"),
            "library": STANDARD.encode(library),
            "catalogs": {},
        });
        let path = data.join(plugin_bundle::PLUGINS_DIR).join(format!(
            "{}.{}",
            state_crypt::opaque_name("plugin-bundle", &id).expect("opaque legacy bundle name"),
            state_crypt::SEALED_EXTENSION
        ));
        fs::create_dir_all(path.parent().expect("legacy plugin bundle directory"))
            .expect("create legacy plugin bundle directory");
        state_crypt::write_state(&path, &bundle.to_string())
            .expect("seed the legacy bundle without an old public API");
    }
    state_crypt::lock();

    for version in versions {
        let sentinel = home.path().join(format!("abi{version}-sentinel"));
        let output = Command::new(&host)
            .arg("--legacy-plugin-abi")
            .arg(format!("abi{version}"))
            .env("HOME", home.path())
            .env("QT_QPA_PLATFORM", "offscreen")
            .env("SILENT_ABI_SENTINEL", &sentinel)
            .output()
            .expect("execute the built Qt legacy ABI fixture");
        let stdout = String::from_utf8_lossy(&output.stdout);
        assert!(
            output.status.success() && stdout.contains("legacy plugin ABI smoke: OK"),
            "legacy ABI {version} fixture failed:\nstdout:\n{stdout}\nstderr:\n{}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(
            stdout.contains(&format!("found dev.silent.IPlugin/{version}.0")),
            "the refusal names ABI {version}:\n{stdout}"
        );
        assert!(!sentinel.exists(), "ABI {version} library never ran");
    }
}
