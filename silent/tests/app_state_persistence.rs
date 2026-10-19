//! Integration test for app-level state persistence.
//!
//! Exercises the `app_set_plugin_enabled` / `app_enabled_plugins` roundtrip and the
//! `app_active_theme` / `app_set_active_theme` getter/setter against an isolated data dir.

use silent::{
    app_active_theme, app_enabled_plugins, app_set_active_theme, app_set_plugin_enabled,
    set_datadir,
};
use temp_dir::TempDir;

#[test]
fn app_state_plugin_and_theme_roundtrip() {
    let tmp = TempDir::new().expect("create temp dir");
    set_datadir(tmp.path().to_path_buf());
    silent::state_crypt::unlock_for_tests();

    // Fresh datadir: no plugins enabled, default theme.
    assert!(
        app_enabled_plugins().is_empty(),
        "fresh datadir should report no enabled plugins, got {:?}",
        app_enabled_plugins()
    );
    assert_eq!(
        app_active_theme(),
        "light",
        "fresh datadir should report the default theme"
    );

    // Enable two plugins, then disable one.
    app_set_plugin_enabled("sp".to_string(), true);
    app_set_plugin_enabled("cj".to_string(), true);
    app_set_plugin_enabled("sp".to_string(), false);

    assert_eq!(
        app_enabled_plugins(),
        vec!["cj".to_string()],
        "sp was disabled, only cj should remain enabled"
    );

    // Re-enabling sp should not duplicate cj.
    app_set_plugin_enabled("sp".to_string(), true);
    let mut plugins = app_enabled_plugins();
    plugins.sort();
    assert_eq!(
        plugins,
        vec!["cj".to_string(), "sp".to_string()],
        "both cj and sp should be enabled with no duplicates"
    );

    // Theme set/get roundtrip.
    app_set_active_theme("solarized".to_string());
    assert_eq!(
        app_active_theme(),
        "solarized",
        "active theme should reflect the value just set"
    );

    // Empty theme normalizes back to the default.
    app_set_active_theme(String::new());
    assert_eq!(
        app_active_theme(),
        "light",
        "setting an empty theme should normalize to the default"
    );
}
