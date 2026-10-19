use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};

use crate::config::datadir;
#[cfg(test)]
use crate::config::set_datadir;
use crate::ffi::{BackendServer, Network, PluginAccess, PluginGrant, QrDisplayLevels};
use crate::state_crypt;

const DEFAULT_THEME: &str = "light";
/// The QR display steppers' middle level, as the C++ side's `qr::DEFAULT_LEVEL`.
const DEFAULT_QR_LEVEL: u8 = 3;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppState {
    #[serde(default)]
    pub enabled_plugins: Vec<String>,
    #[serde(default = "default_active_theme")]
    pub active_theme: String,
    /// The locale picked in Global Settings, empty until one is.
    #[serde(default)]
    pub language: String,
    /// The QR display tuning picked last, as the steppers' levels, for every account.
    #[serde(default = "default_qr_level")]
    pub qr_density: u8,
    #[serde(default = "default_qr_level")]
    pub qr_contrast: u8,
    #[serde(default = "default_qr_level")]
    pub qr_speed: u8,
    /// The camera contrast picked last in Camera Settings, as its stepper's level.
    #[serde(default = "default_qr_level")]
    pub qr_camera_contrast: u8,
    /// The server addresses the last account of each network was created with, by
    /// `"<server>:<network>"`, prefilled in Create Account.
    #[serde(default)]
    pub endpoints: BTreeMap<String, String>,
    /// What each external plugin library may use, by its sha256.
    #[serde(default)]
    pub plugin_access: Vec<StoredAccess>,
}

/// One `PluginAccess`, as `app.json` stores it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct StoredAccess {
    pub plugin_id: String,
    pub hash: String,
    #[serde(default)]
    pub account: StoredGrant,
    #[serde(default)]
    pub keys: StoredGrant,
    #[serde(default)]
    pub state: StoredGrant,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum StoredGrant {
    #[default]
    Unasked,
    Allowed,
    Denied,
}

impl From<StoredGrant> for PluginGrant {
    fn from(grant: StoredGrant) -> Self {
        match grant {
            StoredGrant::Unasked => PluginGrant::Unasked,
            StoredGrant::Allowed => PluginGrant::Allowed,
            StoredGrant::Denied => PluginGrant::Denied,
        }
    }
}

impl From<PluginGrant> for StoredGrant {
    fn from(grant: PluginGrant) -> Self {
        match grant {
            PluginGrant::Allowed => StoredGrant::Allowed,
            PluginGrant::Denied => StoredGrant::Denied,
            _ => StoredGrant::Unasked,
        }
    }
}

fn default_qr_level() -> u8 {
    DEFAULT_QR_LEVEL
}

fn default_active_theme() -> String {
    DEFAULT_THEME.to_string()
}

impl Default for AppState {
    fn default() -> Self {
        Self {
            enabled_plugins: Vec::new(),
            active_theme: default_active_theme(),
            language: String::new(),
            qr_density: DEFAULT_QR_LEVEL,
            qr_contrast: DEFAULT_QR_LEVEL,
            qr_speed: DEFAULT_QR_LEVEL,
            qr_camera_contrast: DEFAULT_QR_LEVEL,
            endpoints: BTreeMap::new(),
            plugin_access: Vec::new(),
        }
    }
}

impl AppState {
    fn path() -> PathBuf {
        datadir().join("app.json")
    }

    pub fn load() -> Self {
        let path = Self::path();
        let content = match state_crypt::read_state(&path) {
            Ok(Some(content)) => content,
            Ok(None) => return Self::default(),
            Err(e) => {
                log::error!("AppState::load() {e}");
                return Self::default();
            }
        };

        match serde_json::from_str::<Self>(&content) {
            Ok(mut state) => {
                state.dedup_plugins();
                if state.active_theme.is_empty() {
                    state.active_theme = default_active_theme();
                }
                state
            }
            Err(e) => {
                log::error!("AppState::load() failed to parse {}: {e}", path.display());
                Self::default()
            }
        }
    }

    pub fn save(&self) {
        let path = Self::path();
        if let Some(parent) = path.parent() {
            let _ = fs::create_dir_all(parent);
        }

        let mut state = self.clone();
        state.dedup_plugins();

        match serde_json::to_string_pretty(&state) {
            Ok(content) => {
                if let Err(e) = state_crypt::write_state(&path, &content) {
                    log::error!("AppState::save() {e}");
                }
            }
            Err(e) => log::error!("AppState::save() failed to serialize: {e}"),
        }
    }

    fn dedup_plugins(&mut self) {
        let mut unique = Vec::with_capacity(self.enabled_plugins.len());
        for plugin_id in self.enabled_plugins.drain(..) {
            if !unique.contains(&plugin_id) {
                unique.push(plugin_id);
            }
        }
        self.enabled_plugins = unique;
    }
}

pub fn app_enabled_plugins() -> Vec<String> {
    AppState::load().enabled_plugins
}

pub fn app_set_plugin_enabled(id: String, enabled: bool) {
    let mut state = AppState::load();

    if enabled {
        if !state.enabled_plugins.contains(&id) {
            state.enabled_plugins.push(id);
        }
    } else {
        state.enabled_plugins.retain(|plugin_id| plugin_id != &id);
    }

    state.save();
}

pub fn app_active_theme() -> String {
    AppState::load().active_theme
}

pub fn app_set_active_theme(name: String) {
    let mut state = AppState::load();
    state.active_theme = if name.is_empty() {
        default_active_theme()
    } else {
        name
    };
    state.save();
}

pub fn app_language() -> String {
    AppState::load().language
}

pub fn app_set_language(locale: String) {
    let mut state = AppState::load();
    state.language = locale;
    state.save();
}

pub fn app_qr_display_levels() -> QrDisplayLevels {
    let state = AppState::load();
    QrDisplayLevels {
        density: state.qr_density,
        contrast: state.qr_contrast,
        speed: state.qr_speed,
    }
}

pub fn app_set_qr_display_levels(levels: QrDisplayLevels) {
    let mut state = AppState::load();
    state.qr_density = levels.density;
    state.qr_contrast = levels.contrast;
    state.qr_speed = levels.speed;
    state.save();
}

pub fn app_qr_camera_contrast_level() -> u8 {
    AppState::load().qr_camera_contrast
}

pub fn app_set_qr_camera_contrast_level(level: u8) {
    let mut state = AppState::load();
    state.qr_camera_contrast = level;
    state.save();
}

fn endpoint_key(server: BackendServer, network: Network) -> String {
    let server = match server {
        BackendServer::Blindbit => "blindbit",
        BackendServer::Electrum => "electrum",
        _ => "unknown",
    };
    let network = match network {
        Network::Bitcoin => "bitcoin",
        Network::Signet => "signet",
        Network::Testnet => "testnet",
        Network::Regtest => "regtest",
        _ => "unknown",
    };
    format!("{server}:{network}")
}

pub fn app_last_endpoint(server: BackendServer, network: Network) -> String {
    AppState::load()
        .endpoints
        .get(&endpoint_key(server, network))
        .cloned()
        .unwrap_or_default()
}

pub fn app_set_last_endpoint(server: BackendServer, network: Network, url: String) {
    let mut state = AppState::load();
    let key = endpoint_key(server, network);
    if url.is_empty() {
        state.endpoints.remove(&key);
    } else {
        state.endpoints.insert(key, url);
    }
    state.save();
}

pub fn app_plugin_access() -> Vec<PluginAccess> {
    AppState::load()
        .plugin_access
        .into_iter()
        .map(|access| PluginAccess {
            plugin_id: access.plugin_id,
            hash: access.hash,
            account: access.account.into(),
            keys: access.keys.into(),
            state: access.state.into(),
        })
        .collect()
}

pub fn app_set_plugin_access(access: PluginAccess) {
    let mut state = AppState::load();
    let stored = StoredAccess {
        plugin_id: access.plugin_id,
        hash: access.hash.to_lowercase(),
        account: access.account.into(),
        keys: access.keys.into(),
        state: access.state.into(),
    };
    let unasked = [stored.account, stored.keys, stored.state]
        .iter()
        .all(|grant| *grant == StoredGrant::Unasked);
    match state
        .plugin_access
        .iter()
        .position(|entry| entry.hash == stored.hash)
    {
        Some(index) if unasked => {
            state.plugin_access.remove(index);
        }
        Some(index) => state.plugin_access[index] = stored,
        None if unasked => {}
        None => state.plugin_access.push(stored),
    }
    state.save();
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Mutex, OnceLock};
    use temp_dir::TempDir;

    // `set_datadir` uses a process-wide `OnceLock`, so every test in this binary shares a
    // single base directory. We create one `TempDir` guard for the process (kept alive for
    // its whole lifetime so it cleans up on exit) and serialize tests with a mutex, resetting
    // `app.json` at the start of each test for isolation.
    static TEST_LOCK: Mutex<()> = Mutex::new(());
    static DATADIR_GUARD: OnceLock<TempDir> = OnceLock::new();

    /// Acquire the test mutex, point `datadir()` at the shared `TempDir`, and clear any
    /// leftover `app.json` so each test starts from a clean slate.
    fn with_isolated_datadir(test: impl FnOnce(PathBuf)) {
        let _guard = TEST_LOCK.lock().expect("test lock poisoned");
        let tmp = DATADIR_GUARD.get_or_init(|| TempDir::new().expect("create temp dir"));
        let base = tmp.path().to_path_buf();
        set_datadir(base.clone());
        let _ = fs::remove_file(base.join("app.json"));
        test(base);
    }

    #[test]
    fn load_missing_file_returns_defaults() {
        with_isolated_datadir(|base| {
            assert!(
                !base.join("app.json").exists(),
                "app.json should be absent at test start"
            );

            let state = AppState::load();

            assert!(
                state.enabled_plugins.is_empty(),
                "missing file should yield no enabled plugins, got {:?}",
                state.enabled_plugins
            );
            assert_eq!(
                state.active_theme, "light",
                "missing file should fall back to the default theme"
            );
            assert!(
                state.language.is_empty(),
                "missing file should pick no language, got {:?}",
                state.language
            );
        });
    }

    #[test]
    fn qr_display_levels_default_then_persist() {
        with_isolated_datadir(|_base| {
            let middle = QrDisplayLevels {
                density: 3,
                contrast: 3,
                speed: 3,
            };
            assert_eq!(
                app_qr_display_levels(),
                middle,
                "a missing file should give the middle level for every setting"
            );

            let picked = QrDisplayLevels {
                density: 1,
                contrast: 5,
                speed: 2,
            };
            app_set_qr_display_levels(picked);
            assert_eq!(
                app_qr_display_levels(),
                picked,
                "the levels picked should be loaded back"
            );
            assert_eq!(
                app_qr_camera_contrast_level(),
                3,
                "the camera contrast should start at the middle level"
            );
            app_set_qr_camera_contrast_level(4);
            assert_eq!(
                app_qr_camera_contrast_level(),
                4,
                "the camera contrast picked should be loaded back"
            );
            assert_eq!(
                app_qr_display_levels(),
                picked,
                "saving the camera contrast should leave the display levels alone"
            );
            assert!(
                AppState::load().language.is_empty(),
                "saving the QR levels should leave the other settings alone"
            );
        });
    }

    #[test]
    fn last_endpoints_are_kept_per_server_and_network() {
        with_isolated_datadir(|_base| {
            assert!(app_last_endpoint(BackendServer::Blindbit, Network::Signet).is_empty());
            app_set_last_endpoint(
                BackendServer::Blindbit,
                Network::Signet,
                "https://signet.example".to_string(),
            );
            app_set_last_endpoint(
                BackendServer::Electrum,
                Network::Signet,
                "ssl://electrum.example:50002".to_string(),
            );
            assert_eq!(
                app_last_endpoint(BackendServer::Blindbit, Network::Signet),
                "https://signet.example"
            );
            assert_eq!(
                app_last_endpoint(BackendServer::Electrum, Network::Signet),
                "ssl://electrum.example:50002"
            );
            assert!(app_last_endpoint(BackendServer::Blindbit, Network::Regtest).is_empty());
            app_set_last_endpoint(BackendServer::Blindbit, Network::Signet, String::new());
            assert!(app_last_endpoint(BackendServer::Blindbit, Network::Signet).is_empty());
        });
    }

    #[test]
    fn plugin_access_is_kept_per_library_hash() {
        with_isolated_datadir(|base| {
            let access = |hash: &str, account, keys| PluginAccess {
                plugin_id: "example".to_string(),
                hash: hash.to_string(),
                account,
                keys,
                state: PluginGrant::Unasked,
            };
            assert!(app_plugin_access().is_empty());
            app_set_plugin_access(access("AA", PluginGrant::Allowed, PluginGrant::Unasked));
            app_set_plugin_access(access("bb", PluginGrant::Denied, PluginGrant::Denied));
            assert_eq!(
                app_plugin_access(),
                vec![
                    access("aa", PluginGrant::Allowed, PluginGrant::Unasked),
                    access("bb", PluginGrant::Denied, PluginGrant::Denied),
                ],
                "each library hash keeps its own answers, the hash stored lowercase"
            );
            app_set_plugin_access(access("aa", PluginGrant::Allowed, PluginGrant::Allowed));
            assert_eq!(
                app_plugin_access()[0],
                access("aa", PluginGrant::Allowed, PluginGrant::Allowed),
                "a new answer replaces the one stored for the same hash"
            );
            let stored = state_crypt::read_state(&base.join("app.json"))
                .expect("read app.json")
                .expect("app.json written");
            assert!(
                stored.contains("\"keys\": \"allowed\""),
                "grants are stored by name: {stored}"
            );
            app_set_plugin_access(access("bb", PluginGrant::Unasked, PluginGrant::Unasked));
            assert_eq!(
                app_plugin_access(),
                vec![access("aa", PluginGrant::Allowed, PluginGrant::Allowed)],
                "forgetting both answers drops the entry"
            );
        });
    }

    #[test]
    fn roundtrip_persists_enabled_plugins_and_theme() {
        with_isolated_datadir(|_base| {
            app_set_plugin_enabled("plugin-a".to_string(), true);
            app_set_plugin_enabled("plugin-a".to_string(), true);
            app_set_plugin_enabled("plugin-b".to_string(), true);
            app_set_plugin_enabled("plugin-a".to_string(), false);
            app_set_active_theme("solarized".to_string());
            app_set_language("fr".to_string());

            let state = AppState::load();

            assert_eq!(
                state.enabled_plugins,
                vec!["plugin-b".to_string()],
                "plugin-a was toggled off, only plugin-b should remain"
            );
            assert_eq!(
                state.active_theme, "solarized",
                "active theme should persist across save/load"
            );
            assert_eq!(
                state.language, "fr",
                "language should persist across save/load"
            );
        });
    }

    #[test]
    fn load_corrupt_json_falls_back_to_default() {
        with_isolated_datadir(|base| {
            state_crypt::write_state(&base.join("app.json"), "{ this is not valid json")
                .expect("write corrupt app.json");

            let state = AppState::load();

            assert!(
                state.enabled_plugins.is_empty(),
                "corrupt JSON should fall back to empty plugins, got {:?}",
                state.enabled_plugins
            );
            assert_eq!(
                state.active_theme, "light",
                "corrupt JSON should fall back to the default theme"
            );
        });
    }

    #[test]
    fn load_empty_theme_normalizes_to_default() {
        with_isolated_datadir(|base| {
            state_crypt::write_state(
                &base.join("app.json"),
                r#"{"enabled_plugins":[],"active_theme":""}"#,
            )
            .expect("write app.json with empty theme");

            let state = AppState::load();

            assert_eq!(
                state.active_theme, "light",
                "empty theme on disk should be normalized to the default"
            );
        });
    }

    #[test]
    fn load_dedups_duplicate_plugin_ids() {
        with_isolated_datadir(|base| {
            state_crypt::write_state(
                &base.join("app.json"),
                r#"{"enabled_plugins":["sp","sp","cj","sp","cj"],"active_theme":"light"}"#,
            )
            .expect("write app.json with duplicate plugins");

            let state = AppState::load();

            assert_eq!(
                state.enabled_plugins,
                vec!["sp".to_string(), "cj".to_string()],
                "duplicate plugin ids should be deduped preserving first-seen order"
            );
        });
    }
}
