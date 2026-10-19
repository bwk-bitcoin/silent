//! A dropped plugin is sealed into the state, listed and opened from there only
//! (ws-plaude/silent#41).

use std::fs;

use silent::plugin_bundle;
use silent::set_datadir;

const SUFFIX: &str = if cfg!(target_os = "macos") {
    "dylib"
} else if cfg!(target_os = "windows") {
    "dll"
} else {
    "so"
};

// One test: the data directory and the unlock key are process wide.
#[test]
fn a_dropped_plugin_is_sealed_and_only_its_bundle_is_read() {
    let state = temp_dir::TempDir::new().expect("state dir");
    set_datadir(state.path().to_path_buf());
    silent::state_crypt::unlock_for_tests();

    // The plugin as built: sidecar, library, catalogs.
    let build = temp_dir::TempDir::new().expect("build dir");
    let dir = build.path().join("example");
    fs::create_dir_all(dir.join("i18n")).unwrap();
    fs::write(
        dir.join("example.json"),
        r#"{"id":"example","name":"Example Plugin","version":"0.2.0"}"#,
    )
    .unwrap();
    let library_name = if cfg!(target_os = "windows") {
        "example.dll".to_string()
    } else {
        format!("libexample.{SUFFIX}")
    };
    fs::write(dir.join(&library_name), b"\x7fELF library v1").unwrap();
    fs::write(
        dir.join("i18n").join("silent_en.lang"),
        "hello => \"Hello\"\n",
    )
    .unwrap();

    // Dropping the folder.
    assert_eq!(plugin_bundle::install(&dir).unwrap(), "example");
    let listed = plugin_bundle::list();
    assert_eq!(listed.len(), 1);
    assert!(listed[0].contains("\"Example Plugin\""));
    let opened = plugin_bundle::open("example").unwrap();
    assert_eq!(opened.library, b"\x7fELF library v1");
    assert_eq!(opened.library_name, library_name);
    assert_eq!(
        opened.catalogs,
        vec![(
            "silent_en.lang".to_string(),
            "hello => \"Hello\"\n".to_string()
        )]
    );

    // Sealed, under an opaque name: no id, no library in clear.
    let plugins = state.path().join(plugin_bundle::PLUGINS_DIR);
    let files: Vec<_> = fs::read_dir(&plugins).unwrap().flatten().collect();
    assert_eq!(files.len(), 1);
    let name = files[0].file_name().to_string_lossy().to_string();
    assert!(name.ends_with(".bip138") && !name.contains("example"));
    let sealed = fs::read(files[0].path()).unwrap();
    assert!(silent::state_crypt::is_sealed(&sealed));
    assert!(!String::from_utf8_lossy(&sealed).contains("Example Plugin"));

    // Dropping the library: its sidecar beside it, a new version replaces the old.
    fs::write(dir.join(&library_name), b"\x7fELF library v2").unwrap();
    assert_eq!(
        plugin_bundle::install(&dir.join(&library_name)).unwrap(),
        "example"
    );
    assert_eq!(plugin_bundle::list().len(), 1);
    assert_eq!(
        plugin_bundle::open("example").unwrap().library,
        b"\x7fELF library v2"
    );

    // A plugin left in clear in the plugins folder is never read.
    let loose = plugins.join("loose");
    fs::create_dir_all(&loose).unwrap();
    fs::write(loose.join("loose.json"), r#"{"id":"loose"}"#).unwrap();
    fs::write(loose.join(format!("libloose.{SUFFIX}")), b"swapped").unwrap();
    assert_eq!(plugin_bundle::list().len(), 1);
    assert!(plugin_bundle::open("loose").is_err());
    fs::remove_dir_all(&loose).unwrap();

    // No sidecar, a bad id: refused.
    let empty = build.path().join("empty");
    fs::create_dir_all(&empty).unwrap();
    assert!(plugin_bundle::install(&empty).is_err());
    let bad = build.path().join("bad");
    fs::create_dir_all(&bad).unwrap();
    fs::write(bad.join("bad.json"), r#"{"id":"../escape"}"#).unwrap();
    fs::write(bad.join(format!("libbad.{SUFFIX}")), b"x").unwrap();
    assert!(plugin_bundle::install(&bad).is_err());

    plugin_bundle::remove("example").unwrap();
    assert!(plugin_bundle::list().is_empty());
}
