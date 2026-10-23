use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};

#[cfg(test)]
use crate::config::set_datadir;
use crate::{
    config::datadir,
    ffi::{
        BackendServer, Network, PluginAccess, PluginAccessHistory, PluginAccessRequest,
        PluginGrant, QrDisplayLevels, RustFeePreferences,
    },
    state_crypt,
};

const DEFAULT_THEME: &str = "light";
/// The QR display steppers' middle level, as the C++ side's `qr::DEFAULT_LEVEL`.
const DEFAULT_QR_LEVEL: u8 = 3;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppState {
    #[serde(default)]
    pub enabled_plugins: Vec<String>,
    #[serde(default)]
    pub removing_plugins: BTreeMap<String, RemovalPhase>,
    /// Retained across uninstall so a previous installation's handles never revive.
    #[serde(default)]
    pub storage_epochs: BTreeMap<String, u64>,
    #[serde(default = "default_active_theme")]
    pub active_theme: String,
    /// The locale picked in Global Settings, empty until one is.
    #[serde(default)]
    pub language: String,
    #[serde(default)]
    pub price_source: String,
    #[serde(default)]
    pub price_currency: String,
    #[serde(default = "default_fee_preferences")]
    pub fee_preferences: FeePreferencesInternal,
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

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RemovalPhase {
    Stopping,
    PrivateData,
    Bundle,
    Bookkeeping,
}

impl From<RemovalPhase> for crate::ffi::PluginRemovalPhase {
    fn from(phase: RemovalPhase) -> Self {
        match phase {
            RemovalPhase::Stopping => Self::Stopping,
            RemovalPhase::PrivateData => Self::PrivateData,
            RemovalPhase::Bundle => Self::Bundle,
            RemovalPhase::Bookkeeping => Self::Bookkeeping,
        }
    }
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

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct FeePreferencesInternal {
    mode: u8,
    default_source: String,
    smart_source: String,
    tier: u8,
}

fn default_fee_preferences() -> FeePreferencesInternal {
    FeePreferencesInternal {
        tier: crate::feed::DEFAULT_FEE_TIER,
        ..FeePreferencesInternal::default()
    }
}

fn default_active_theme() -> String {
    DEFAULT_THEME.to_string()
}

impl Default for AppState {
    fn default() -> Self {
        Self {
            enabled_plugins: Vec::new(),
            removing_plugins: BTreeMap::new(),
            storage_epochs: BTreeMap::new(),
            active_theme: default_active_theme(),
            language: String::new(),
            price_source: String::new(),
            price_currency: String::new(),
            fee_preferences: default_fee_preferences(),
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
        datadir().join("app.bip138")
    }

    pub fn load() -> Self {
        match Self::try_load() {
            Ok(state) => state,
            Err(error) => {
                log::error!("AppState::load() {error}");
                Self::default()
            }
        }
    }

    pub(crate) fn try_load() -> Result<Self, String> {
        Self::try_load_in(&datadir())
    }

    pub(crate) fn try_load_in(data_dir: &Path) -> Result<Self, String> {
        let path = data_dir.join("app.bip138");
        let content = match state_crypt::read_state(&path)? {
            Some(content) => content,
            None => return Ok(Self::default()),
        };
        let mut state: Self = serde_json::from_str(&content)
            .map_err(|e| format!("failed to parse {}: {e}", path.display()))?;
        state.dedup_plugins();
        if state.active_theme.is_empty() {
            state.active_theme = default_active_theme();
        }
        Ok(state)
    }

    // Only transaction holders may publish; never expose snapshot saving to callers.
    fn try_save(&self) -> Result<(), String> {
        let path = Self::path();
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }

        let mut state = self.clone();
        state.dedup_plugins();

        let content = serde_json::to_string_pretty(&state).map_err(|e| e.to_string())?;
        state_crypt::write_state_durable(&path, &content)
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
    AppState::load().enabled_plugin_ids()
}

impl AppState {
    fn enabled_plugin_ids(&self) -> Vec<String> {
        self.enabled_plugins
            .iter()
            .filter(|id| !self.removing_plugins.contains_key(*id))
            .cloned()
            .collect()
    }
}

pub(crate) fn plugin_removal_pending(id: &str) -> Result<bool, String> {
    plugin_removal_phase(id).map(|phase| phase.is_some())
}

pub(crate) fn plugin_removal_phase(id: &str) -> Result<Option<RemovalPhase>, String> {
    AppState::try_load().map(|state| state.removing_plugins.get(id).copied())
}

#[cfg(test)]
pub(crate) fn advance_plugin_removal(id: &str, phase: RemovalPhase) -> Result<(), String> {
    let _publication = state_crypt::publication_change_guard();
    let _state = crate::storage::app_state_lock(&datadir())?;
    advance_plugin_removal_locked(id, phase)
}

pub(crate) fn advance_plugin_removal_locked(id: &str, phase: RemovalPhase) -> Result<(), String> {
    let mut state = AppState::try_load()?;
    let current = *state
        .removing_plugins
        .get(id)
        .ok_or("plugin removal was not prepared")?;
    let next = current.max(phase);
    if next >= RemovalPhase::PrivateData
        && (current == RemovalPhase::Stopping || !state.storage_epochs.contains_key(id))
    {
        let epoch = state.storage_epochs.entry(id.into()).or_default();
        *epoch = epoch
            .checked_add(1)
            .ok_or("plugin storage epoch exhausted")?;
    }
    state.removing_plugins.insert(id.into(), next);
    // Repeat the durable save even after a previous save failed after rename.
    state.try_save()
}

pub fn app_begin_plugin_removal(id: String) -> String {
    begin_plugin_removal(&id).err().unwrap_or_default()
}

pub fn app_disable_plugin(id: String) -> String {
    let disable = || -> Result<(), String> {
        let _publication = state_crypt::publication_change_guard();
        let _state = crate::storage::app_state_lock(&datadir())?;
        let mut state = AppState::try_load()?;
        state.enabled_plugins.retain(|plugin| plugin != &id);
        state.try_save()
    };
    disable().err().unwrap_or_default()
}

pub fn app_removing_plugins() -> crate::ffi::PluginRemovalListResult {
    let load = || -> Result<AppState, String> {
        let _publication = state_crypt::publication_guard();
        if !state_crypt::holds_key() {
            return Err(
                "Application state is locked. Unlock it, then retry reading pending removals."
                    .into(),
            );
        }
        let _state = crate::storage::app_state_lock(&datadir())?;
        AppState::try_load()
    };
    match load() {
        Ok(state) => crate::ffi::PluginRemovalListResult {
            is_ok: true,
            error: String::new(),
            enabled_plugins: state.enabled_plugin_ids(),
            entries: state
                .removing_plugins
                .into_iter()
                .map(|(plugin_id, phase)| crate::ffi::PluginRemovalEntry {
                    plugin_id,
                    phase: phase.into(),
                })
                .collect(),
        },
        Err(error) => crate::ffi::PluginRemovalListResult {
            is_ok: false,
            error,
            entries: Vec::new(),
            enabled_plugins: Vec::new(),
        },
    }
}

fn begin_plugin_removal(id: &str) -> Result<(), String> {
    if !state_crypt::valid_plugin_id(id) {
        return Err("invalid plugin id".into());
    }
    let _publication = state_crypt::publication_change_guard();
    let _state = crate::storage::app_state_lock(&datadir())?;
    let mut state = AppState::try_load()?;
    state.enabled_plugins.retain(|plugin| plugin != id);
    state
        .removing_plugins
        .entry(id.into())
        .or_insert(RemovalPhase::Stopping);
    state.try_save()
}

#[cfg(test)]
pub(crate) fn finish_plugin_removal(id: &str) -> Result<(), String> {
    let _publication = state_crypt::publication_change_guard();
    let _state = crate::storage::app_state_lock(&datadir())?;
    finish_plugin_removal_locked(id)
}

pub(crate) fn finish_plugin_removal_locked(id: &str) -> Result<(), String> {
    let mut state = AppState::try_load()?;
    if state.removing_plugins.get(id) != Some(&RemovalPhase::Bookkeeping) {
        return Err("plugin removal has not reached final bookkeeping".into());
    }
    let hashes: Vec<_> = state
        .plugin_access
        .iter()
        .filter(|access| access.plugin_id == id)
        .map(|access| access.hash.clone())
        .collect();
    state.enabled_plugins.retain(|plugin| plugin != id);
    state.plugin_access.retain(|access| access.plugin_id != id);
    state.removing_plugins.remove(id);
    let saved = state.try_save();
    let published = match &saved {
        Ok(()) => true,
        Err(_) => match AppState::try_load() {
            Ok(published) => {
                !published.removing_plugins.contains_key(id)
                    && !published
                        .plugin_access
                        .iter()
                        .any(|access| access.plugin_id == id)
            }
            Err(error) => {
                log::error!("cannot verify failed removal bookkeeping publication: {error}");
                true
            }
        },
    };
    if published {
        for hash in hashes {
            crate::storage::grants_changed(&hash);
        }
    }
    saved
}

pub fn app_set_plugin_enabled(id: String, enabled: bool) -> String {
    let persist = || -> Result<(), String> {
        let _publication = state_crypt::publication_change_guard();
        let _state = crate::storage::app_state_lock(&datadir())?;
        let mut state = AppState::try_load()?;
        if enabled && state.removing_plugins.contains_key(&id) {
            return Err("Plugin removal is pending; finish cleanup before enabling".into());
        }
        if enabled {
            if !state.enabled_plugins.contains(&id) {
                state.enabled_plugins.push(id);
            }
        } else {
            state.enabled_plugins.retain(|plugin_id| plugin_id != &id);
        }
        state.try_save()
    };
    persist().err().unwrap_or_default()
}

fn update_settings(change: impl FnOnce(&mut AppState)) {
    let persist = || -> Result<(), String> {
        let _publication = state_crypt::publication_change_guard();
        let _state = crate::storage::app_state_lock(&datadir())?;
        let mut state = AppState::try_load()?;
        change(&mut state);
        state.try_save()
    };
    if let Err(error) = persist() {
        log::error!("failed to update application state: {error}");
    }
}

pub fn app_active_theme() -> String {
    AppState::load().active_theme
}

pub fn app_set_active_theme(name: String) {
    update_settings(|state| {
        state.active_theme = if name.is_empty() {
            default_active_theme()
        } else {
            name
        };
    });
}

pub fn app_language() -> String {
    AppState::load().language
}

pub fn app_price_source() -> String {
    AppState::load().price_source
}

pub fn app_fee_preferences() -> RustFeePreferences {
    let preferences = AppState::load().fee_preferences;
    RustFeePreferences {
        mode: preferences.mode,
        default_source: preferences.default_source,
        smart_source: preferences.smart_source,
        tier: preferences.tier,
    }
}

pub fn app_set_fee_preferences(preferences: RustFeePreferences) {
    if !preferences.is_valid() {
        log::error!("invalid fee mode or tier");
        return;
    }
    update_settings(|state| {
        state.fee_preferences = FeePreferencesInternal {
            mode: preferences.mode,
            default_source: preferences.default_source,
            smart_source: preferences.smart_source,
            tier: preferences.tier,
        };
    });
}

pub fn app_price_currency() -> String {
    AppState::load().price_currency
}

pub fn app_set_price_selection(source: String, currency: String) {
    update_settings(|state| {
        state.price_source = source;
        state.price_currency = crate::feed::currency(&currency);
    });
}

pub fn app_set_language(locale: String) {
    update_settings(|state| {
        state.language = locale;
    });
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
    update_settings(|state| {
        state.qr_density = levels.density;
        state.qr_contrast = levels.contrast;
        state.qr_speed = levels.speed;
    });
}

pub fn app_qr_camera_contrast_level() -> u8 {
    AppState::load().qr_camera_contrast
}

pub fn app_set_qr_camera_contrast_level(level: u8) {
    update_settings(|state| {
        state.qr_camera_contrast = level;
    });
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
    update_settings(|state| {
        let key = endpoint_key(server, network);
        if url.is_empty() {
            state.endpoints.remove(&key);
        } else {
            state.endpoints.insert(key, url);
        }
    });
}

pub(crate) fn remember_endpoints(
    network: Network,
    endpoints: &[(BackendServer, String)],
) -> Result<(), String> {
    let _publication = state_crypt::publication_change_guard();
    let _state = crate::storage::app_state_lock(&datadir())?;
    let mut state = AppState::try_load()?;
    for (server, url) in endpoints {
        state
            .endpoints
            .insert(endpoint_key(*server, network), url.clone());
    }
    state.try_save()
}

impl From<StoredAccess> for PluginAccess {
    fn from(access: StoredAccess) -> Self {
        Self {
            plugin_id: access.plugin_id,
            hash: access.hash,
            account: access.account.into(),
            keys: access.keys.into(),
            state: access.state.into(),
        }
    }
}

impl AppState {
    /// The answers kept for the library `hash`, every grant unasked when none are.
    fn plugin_answers(&self, plugin_id: String, hash: String) -> PluginAccess {
        let hash = hash.to_ascii_lowercase();
        self.plugin_access
            .iter()
            .find(|entry| entry.hash == hash)
            .cloned()
            .map(PluginAccess::from)
            .unwrap_or(PluginAccess {
                plugin_id,
                hash,
                account: PluginGrant::Unasked,
                keys: PluginGrant::Unasked,
                state: PluginGrant::Unasked,
            })
    }

    fn plugin_history(&self, plugin_id: &str) -> Vec<PluginAccess> {
        self.plugin_access
            .iter()
            .filter(|entry| entry.plugin_id == plugin_id)
            .cloned()
            .map(PluginAccess::from)
            .collect()
    }

    fn storage_epoch(&self, plugin_id: &str) -> u64 {
        self.storage_epochs.get(plugin_id).copied().unwrap_or(0)
    }

    /// No access answer of `plugin_id` is asked or saved while its removal is pending.
    fn refuse_pending_removal(&self, plugin_id: &str) -> Result<(), String> {
        if self.removing_plugins.contains_key(plugin_id) {
            return Err("Plugin removal is pending: access answers cannot change".into());
        }
        Ok(())
    }
}

/// Reads the app state for a grant request of `plugin_id`, with its storage epoch, under
/// the same locks as a grant save.
fn read_for_grant<T>(
    plugin_id: &str,
    read: impl FnOnce(&AppState) -> T,
) -> Result<(u64, T), String> {
    let _publication = state_crypt::publication_guard();
    if !state_crypt::holds_key() {
        return Err("Application state is locked. Unlock it and retry access".into());
    }
    let _state = crate::storage::app_state_lock(&datadir())?;
    let state = AppState::try_load()?;
    state.refuse_pending_removal(plugin_id)?;
    Ok((state.storage_epoch(plugin_id), read(&state)))
}

pub fn app_plugin_access_request(plugin_id: String, hash: String) -> PluginAccessRequest {
    let read = read_for_grant(&plugin_id, |state| {
        let mut access = state.plugin_answers(plugin_id.clone(), hash.clone());
        access.plugin_id = plugin_id.clone();
        access
    });
    match read {
        Ok((storage_epoch, access)) => PluginAccessRequest {
            is_ok: true,
            error: String::new(),
            storage_epoch,
            access,
        },
        Err(error) => PluginAccessRequest {
            is_ok: false,
            error,
            storage_epoch: 0,
            access: PluginAccess {
                plugin_id,
                hash,
                account: PluginGrant::Unasked,
                keys: PluginGrant::Unasked,
                state: PluginGrant::Unasked,
            },
        },
    }
}

pub fn app_plugin_access_history(plugin_id: String) -> PluginAccessHistory {
    match read_for_grant(&plugin_id, |state| state.plugin_history(&plugin_id)) {
        Ok((storage_epoch, entries)) => PluginAccessHistory {
            is_ok: true,
            error: String::new(),
            storage_epoch,
            entries,
        },
        Err(error) => PluginAccessHistory {
            is_ok: false,
            error,
            storage_epoch: 0,
            entries: Vec::new(),
        },
    }
}

pub fn app_plugin_access() -> Vec<PluginAccess> {
    AppState::load()
        .plugin_access
        .into_iter()
        .map(PluginAccess::from)
        .collect()
}

pub fn app_plugin_answers(plugin_id: String, hash: String) -> PluginAccess {
    AppState::load().plugin_answers(plugin_id, hash)
}

pub fn app_plugin_grants(hash: String) -> crate::ffi::PluginGrantedAccess {
    let answer = app_plugin_answers(String::new(), hash);
    crate::ffi::PluginGrantedAccess {
        account: answer.account == PluginGrant::Allowed,
        keys: answer.keys == PluginGrant::Allowed,
        state: answer.state == PluginGrant::Allowed,
    }
}

pub fn app_plugin_history(plugin_id: String) -> Vec<PluginAccess> {
    AppState::load().plugin_history(&plugin_id)
}

pub fn app_set_plugin_access(access: PluginAccess, expected_epoch: u64) -> String {
    try_set_plugin_access(access, expected_epoch)
        .err()
        .unwrap_or_default()
}

fn try_set_plugin_access(access: PluginAccess, expected_epoch: u64) -> Result<(), String> {
    let _publication = state_crypt::publication_change_guard();
    if !state_crypt::holds_key() {
        return Err("Application state is locked. Unlock it and retry access".into());
    }
    let _state = crate::storage::app_state_lock(&datadir())?;
    let mut state = AppState::try_load()?;
    state.refuse_pending_removal(&access.plugin_id)?;
    if state.storage_epoch(&access.plugin_id) != expected_epoch {
        return Err(
            "The plugin was removed since these access answers were read: they were not saved"
                .into(),
        );
    }
    let hash = access.hash.to_ascii_lowercase();
    let previous = state
        .plugin_access
        .iter()
        .find(|entry| entry.hash == hash)
        .cloned();
    let stored = StoredAccess {
        plugin_id: access.plugin_id,
        hash: hash.clone(),
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
    let current = state
        .plugin_access
        .iter()
        .find(|entry| entry.hash == hash)
        .cloned();
    let saved = state.try_save();
    if saved.is_ok() {
        crate::storage::grants_changed(&hash);
    } else if previous != current {
        match AppState::try_load() {
            Ok(published) => {
                if published
                    .plugin_access
                    .iter()
                    .find(|entry| entry.hash == hash)
                    == current.as_ref()
                {
                    crate::storage::grants_changed(&hash);
                }
            }
            Err(error) => {
                log::error!("cannot verify failed grant publication: {error}");
                crate::storage::grants_changed(&hash);
            }
        }
    }
    saved
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        sync::{mpsc, Mutex, OnceLock},
        thread,
        time::Duration,
    };
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
        state_crypt::unlock_for_tests();
        let _ = fs::remove_file(base.join("app.bip138"));
        test(base);
    }

    /// Saves `access` against the storage epoch read when its request starts.
    fn save_access(access: PluginAccess) {
        let request = app_plugin_access_request(access.plugin_id.clone(), access.hash.clone());
        assert!(request.is_ok, "grant request: {}", request.error);
        assert_eq!(
            app_set_plugin_access(access, request.storage_epoch),
            "",
            "grant saved"
        );
    }

    #[test]
    fn removal_epoch_rotates_once_at_the_published_private_phase_and_survives_cleanup() {
        with_isolated_datadir(|_base| {
            state_crypt::unlock_for_tests();
            let id = "epoch-durability";
            begin_plugin_removal(id).expect("stopping checkpoint");
            state_crypt::fail_durable_write_at(state_crypt::DurableWriteStage::TempSynced);
            assert!(advance_plugin_removal(id, RemovalPhase::PrivateData).is_err());
            assert_eq!(
                plugin_removal_phase(id).expect("unpublished phase"),
                Some(RemovalPhase::Stopping)
            );
            assert!(!AppState::try_load()
                .expect("unpublished epoch")
                .storage_epochs
                .contains_key(id));
            state_crypt::fail_durable_write_at(state_crypt::DurableWriteStage::Renamed);
            assert!(advance_plugin_removal(id, RemovalPhase::PrivateData).is_err());
            assert_eq!(
                AppState::try_load()
                    .expect("published epoch")
                    .storage_epochs
                    .get(id),
                Some(&1)
            );
            advance_plugin_removal(id, RemovalPhase::PrivateData).expect("durable retry");
            app_set_language("fr".into());
            assert_eq!(
                AppState::try_load()
                    .expect("epoch after retry/settings")
                    .storage_epochs
                    .get(id),
                Some(&1)
            );
            assert!(
                crate::plugin_bundle::plugin_remove(id.into()).is_empty(),
                "synchronous removal completes"
            );
            assert_eq!(
                AppState::try_load()
                    .expect("retained epoch")
                    .storage_epochs
                    .get(id),
                Some(&1)
            );
            begin_plugin_removal(id).expect("next lifetime stopping");
            advance_plugin_removal(id, RemovalPhase::PrivateData)
                .expect("next lifetime private phase");
            assert_eq!(
                AppState::try_load()
                    .expect("next epoch")
                    .storage_epochs
                    .get(id),
                Some(&2)
            );
        });
    }

    #[cfg(unix)]
    #[test]
    fn removal_journal_retains_private_phase_until_absent_unlink_retries_are_synced() {
        with_isolated_datadir(|base| {
            state_crypt::unlock_for_tests();
            for staging in [false, true] {
                let id = if staging {
                    "staging-durability"
                } else {
                    "legacy-durability"
                };
                let path = if staging {
                    let root = base.join(id);
                    fs::create_dir(&root).expect("staging root");
                    root.join("tree.tmp")
                } else {
                    let root = base.join(state_crypt::PLUGIN_STATE_DIR);
                    fs::create_dir_all(&root).expect("legacy root");
                    root.join(state_crypt::legacy_plugin_state_name(id).expect("legacy name"))
                };
                fs::write(&path, b"unpublished partial data").expect("cleanup fixture");
                begin_plugin_removal(id).expect("durable stopping checkpoint");
                for attempt in 0..2 {
                    state_crypt::fail_durable_write_at(state_crypt::DurableWriteStage::Unlinked);
                    let error = crate::plugin_bundle::plugin_remove(id.into());
                    assert!(!error.is_empty(), "attempt {attempt}: incomplete directory sync cannot report successful removal");
                    assert!(!path.exists(), "unlink itself has already completed");
                    assert_eq!(
                        plugin_removal_phase(id).expect("retry journal"),
                        Some(RemovalPhase::PrivateData),
                        "attempt {attempt}: cleanup cannot advance past unsynced deletion"
                    );
                }
                assert!(
                    crate::plugin_bundle::plugin_remove(id.into()).is_empty(),
                    "retry completes the directory sync before clearing bookkeeping"
                );
                assert_eq!(plugin_removal_phase(id).expect("finished journal"), None);
                assert_eq!(
                    AppState::try_load()
                        .expect("retained epoch")
                        .storage_epochs
                        .get(id),
                    Some(&1)
                );
            }
        });
    }

    #[test]
    #[cfg(unix)]
    fn pending_removal_read_errors_are_explicit_and_recover_without_losing_the_journal() {
        use std::os::unix::fs::PermissionsExt;

        for fault in ["locked", "corrupt", "unreadable", "unwritable-lock"] {
            with_isolated_datadir(|base| {
                state_crypt::unlock_for_tests();
                let id = "pending-read-failure";
                begin_plugin_removal(id).expect("durable pending removal");
                assert_eq!(app_set_plugin_enabled("unrelated".into(), true), "");
                let path = base.join("app.bip138");
                let lock = base.join("app.lock");
                let before = fs::read(&path).expect("original removal ciphertext");
                let state_permissions = fs::metadata(&path)
                    .expect("state permissions")
                    .permissions();
                let lock_permissions = fs::metadata(&lock).expect("lock permissions").permissions();
                match fault {
                    "locked" => state_crypt::lock(),
                    "corrupt" => fs::write(&path, b"corrupt journal").expect("corrupt fixture"),
                    "unreadable" => fs::set_permissions(&path, fs::Permissions::from_mode(0o0))
                        .expect("unreadable fixture"),
                    "unwritable-lock" => {
                        fs::set_permissions(&lock, fs::Permissions::from_mode(0o400))
                            .expect("unwritable lock fixture")
                    }
                    _ => unreachable!("only declared fault cases"),
                }
                let failed = app_removing_plugins();
                assert!(
                    !failed.is_ok,
                    "{fault}: read failure must not mean no pending removal"
                );
                assert!(
                    !failed.error.is_empty(),
                    "{fault}: caller receives the real failure"
                );
                assert!(
                    failed.entries.is_empty(),
                    "{fault}: failed result contains no usable snapshot"
                );
                assert!(
                    failed.enabled_plugins.is_empty(),
                    "{fault}: failed result contains no usable enabled-state snapshot"
                );
                assert!(
                    !app_set_plugin_enabled(id.into(), true).is_empty(),
                    "{fault}: enabling cannot bypass the journal"
                );
                // Production-key locked removal is covered by the real Qt probe, not the unit-test key fallback.
                if fault != "locked" {
                    assert!(
                        !crate::plugin_bundle::plugin_remove(id.into()).is_empty(),
                        "{fault}: removal cannot claim success on unreadable authorization"
                    );
                }
                match fault {
                    "locked" => state_crypt::unlock_for_tests(),
                    "corrupt" => fs::write(&path, &before).expect("restore original ciphertext"),
                    "unreadable" => {
                        fs::set_permissions(&path, state_permissions).expect("restore read access")
                    }
                    "unwritable-lock" => {
                        fs::set_permissions(&lock, lock_permissions).expect("restore lock access")
                    }
                    _ => unreachable!("only declared fault cases"),
                }
                assert_eq!(
                    fs::read(&path).expect("recovered journal"),
                    before,
                    "{fault}: refusal preserves the record bytes"
                );
                let recovered = app_removing_plugins();
                assert!(
                    recovered.is_ok,
                    "{fault}: retry succeeds after access is restored: {}",
                    recovered.error
                );
                assert!(recovered.error.is_empty());
                assert_eq!(recovered.entries.len(), 1);
                assert_eq!(recovered.entries[0].plugin_id, id);
                assert_eq!(
                    recovered.entries[0].phase,
                    crate::ffi::PluginRemovalPhase::Stopping
                );
                assert_eq!(
                    recovered.enabled_plugins,
                    vec!["unrelated".to_string()],
                    "{fault}: enabled state and removals come from one checked load"
                );
            });
        }
    }

    #[test]
    fn grant_and_endpoint_updates_wait_before_loading_their_snapshots() {
        with_isolated_datadir(|base| {
            let access = |state| PluginAccess {
                plugin_id: "example".into(),
                hash: "publication-test".into(),
                account: PluginGrant::Unasked,
                keys: PluginGrant::Unasked,
                state,
            };
            save_access(access(PluginGrant::Allowed));
            let before = fs::read(base.join("app.bip138")).expect("original granted state");
            let publication = crate::state_crypt::publication_guard();
            let (started_tx, started_rx) = mpsc::channel();
            let (done_tx, done_rx) = mpsc::channel();
            let grant_started = started_tx.clone();
            let grant_done = done_tx.clone();
            let revoker = thread::spawn(move || {
                grant_started.send(()).expect("grant update started");
                save_access(access(PluginGrant::Denied));
                grant_done.send(()).expect("grant update completed");
            });
            let settings = thread::spawn(move || {
                started_tx.send(()).expect("endpoint update started");
                remember_endpoints(
                    Network::Regtest,
                    &[(BackendServer::Electrum, "local-test:50001".into())],
                )
                .expect("endpoint update persisted");
                done_tx.send(()).expect("endpoint update completed");
            });
            for _ in 0..2 {
                started_rx
                    .recv_timeout(Duration::from_secs(5))
                    .expect("state updater attempting the barrier");
            }
            assert!(
                matches!(
                    done_rx.recv_timeout(Duration::from_millis(100)),
                    Err(mpsc::RecvTimeoutError::Timeout)
                ),
                "neither state updater may save while an authorized publication holds the barrier"
            );
            assert_eq!(
                fs::read(base.join("app.bip138")).expect("state during publication"),
                before,
                "state remains unchanged until publication finishes"
            );
            drop(publication);
            revoker
                .join()
                .expect("grant update finishes after publication");
            settings
                .join()
                .expect("endpoint update finishes after publication");
            assert!(
                !app_plugin_grants("publication-test".into()).state,
                "a settings snapshot never restores the old grant"
            );
            assert_eq!(
                app_last_endpoint(BackendServer::Electrum, Network::Regtest),
                "local-test:50001",
                "the grant snapshot never loses the endpoint update"
            );
        });
    }

    #[test]
    fn plugin_removal_journal_disables_until_cleanup_and_preserves_selections() {
        with_isolated_datadir(|_base| {
            state_crypt::unlock_for_tests();
            let original = AppState {
                enabled_plugins: vec!["example".into(), "unrelated".into()],
                price_source: "example.prices".into(),
                plugin_access: vec![
                    StoredAccess {
                        plugin_id: "example".into(),
                        hash: "old".into(),
                        account: StoredGrant::Allowed,
                        keys: StoredGrant::Denied,
                        state: StoredGrant::Allowed,
                    },
                    StoredAccess {
                        plugin_id: "unrelated".into(),
                        hash: "other".into(),
                        account: StoredGrant::Denied,
                        keys: StoredGrant::Allowed,
                        state: StoredGrant::Denied,
                    },
                ],
                ..AppState::default()
            };
            original.try_save().expect("original app state");
            begin_plugin_removal("example").expect("durable removal marker");
            assert!(
                plugin_removal_pending("example").expect("read checkpoint"),
                "removal survives a state reload"
            );
            assert!(
                !app_set_plugin_enabled("example".into(), true).is_empty(),
                "pending removal explicitly refuses enable"
            );
            assert_eq!(
                app_enabled_plugins(),
                vec!["unrelated"],
                "pending removal cannot be re-enabled"
            );
            assert_eq!(
                AppState::try_load().expect("pending state").plugin_access,
                original.plugin_access,
                "grants remain until cleanup succeeds"
            );
            assert!(
                finish_plugin_removal("example").is_err(),
                "stopping alone is not permission to clear the journal"
            );
            advance_plugin_removal("example", RemovalPhase::PrivateData).expect("private phase");
            advance_plugin_removal("example", RemovalPhase::Bundle).expect("bundle phase");
            advance_plugin_removal("example", RemovalPhase::Bookkeeping)
                .expect("bookkeeping phase");
            finish_plugin_removal("example").expect("cleanup bookkeeping");
            let finished = AppState::try_load().expect("finished state");
            assert!(
                finished.removing_plugins.is_empty(),
                "successful cleanup clears removal marker"
            );
            assert_eq!(
                finished.plugin_access,
                vec![original.plugin_access[1].clone()],
                "only the removed plugin's hash-specific history is erased"
            );
            assert_eq!(
                finished.price_source, "example.prices",
                "missing selected provider is retained without fallback"
            );
        });
    }

    #[test]
    fn plugin_removal_checkpoint_and_cleanup_save_failures_are_retryable() {
        with_isolated_datadir(|base| {
            state_crypt::unlock_for_tests();
            let state = AppState {
                enabled_plugins: vec!["example".into()],
                ..AppState::default()
            };
            state.try_save().expect("original state");
            let original = fs::read(base.join("app.bip138")).expect("original app bytes");
            state_crypt::fail_durable_write_at(state_crypt::DurableWriteStage::TempSynced);
            assert!(
                !app_begin_plugin_removal("example".into()).is_empty(),
                "checkpoint save failure must be returned"
            );
            assert_eq!(
                fs::read(base.join("app.bip138")).expect("state after failed checkpoint"),
                original,
                "failed checkpoint publishes no partial app state"
            );
            assert!(
                !plugin_removal_pending("example").expect("read failed checkpoint"),
                "an uncommitted checkpoint is not success"
            );
            assert!(
                app_begin_plugin_removal("example".into()).is_empty(),
                "checkpoint retry succeeds"
            );
            advance_plugin_removal("example", RemovalPhase::PrivateData).expect("private phase");
            advance_plugin_removal("example", RemovalPhase::Bundle).expect("bundle phase");
            advance_plugin_removal("example", RemovalPhase::Bookkeeping)
                .expect("bookkeeping phase");
            begin_plugin_removal("example").expect("retry existing checkpoint");
            assert_eq!(
                plugin_removal_phase("example").expect("read retained phase"),
                Some(RemovalPhase::Bookkeeping),
                "retry never downgrades erasure/publication fencing"
            );
            let pending = fs::read(base.join("app.bip138")).expect("pending app bytes");
            state_crypt::fail_durable_write_at(state_crypt::DurableWriteStage::TempSynced);
            assert!(
                finish_plugin_removal("example").is_err(),
                "cleanup save failure must propagate"
            );
            assert_eq!(
                fs::read(base.join("app.bip138")).expect("state after failed cleanup"),
                pending,
                "failed cleanup keeps the complete retry journal"
            );
            assert!(
                plugin_removal_pending("example").expect("read retained checkpoint"),
                "cleanup save failure keeps enable/reinstall blocked"
            );
            finish_plugin_removal("example").expect("cleanup save retry");
            assert!(
                !plugin_removal_pending("example").expect("read completed checkpoint"),
                "retry completes bookkeeping"
            );
        });
    }

    #[test]
    fn unreadable_removal_state_is_not_proof_of_a_committed_checkpoint() {
        with_isolated_datadir(|base| {
            state_crypt::unlock_for_tests();
            let private = base.join("example/tree.bip138");
            state_crypt::write_state(&private, "private ciphertext fixture")
                .expect("private data fixture");
            let before = fs::read(&private).expect("original private bytes");
            fs::write(base.join("app.bip138"), b"not encrypted").expect("unreadable state fixture");
            assert!(
                plugin_removal_pending("example").is_err(),
                "unreadable checkpoint queries fail explicitly"
            );
            let error = crate::plugin_bundle::plugin_remove("example".into());
            assert!(
                error.contains("not encrypted"),
                "destructive removal reports the real checkpoint read failure: {error}"
            );
            assert_eq!(
                fs::read(&private).expect("private bytes after refusal"),
                before,
                "an unreadable journal cannot authorize private-data erasure"
            );
            AppState::default()
                .try_save()
                .expect("repair state fixture");
            assert!(
                crate::plugin_bundle::plugin_remove("example".into()).contains("not prepared"),
                "absence of a checkpoint also refuses erasure"
            );
            assert_eq!(
                fs::read(&private).expect("private bytes without checkpoint"),
                before
            );
        });
    }

    #[test]
    fn plugin_removal_allows_owned_final_writes_then_fences_erasure_phase() {
        with_isolated_datadir(|_base| {
            state_crypt::unlock_for_tests();
            let id = "phase-persistence";
            let hash = "f".repeat(64);
            save_access(PluginAccess {
                plugin_id: id.into(),
                hash: hash.clone(),
                account: PluginGrant::Unasked,
                keys: PluginGrant::Unasked,
                state: PluginGrant::Allowed,
            });
            let session = crate::storage::storage_session_open(id.into(), hash, true);
            assert_ne!(session, 0, "the pre-existing authenticated session opens");
            let send = |message: serde_json::Value| -> serde_json::Value {
                serde_json::from_str(&crate::storage::storage_request(
                    session,
                    message.to_string(),
                ))
                .expect("storage response JSON")
            };
            let opened = send(serde_json::json!({"id":"open", "op":"open", "storage":"private"}));
            let storage = opened["storage"]
                .as_str()
                .expect("owned storage handle")
                .to_string();
            begin_plugin_removal(id).expect("durable stopping checkpoint");
            let final_write = send(
                serde_json::json!({"id":"final", "op":"write", "storage":storage, "path":"account", "data":"ZmluYWw=", "mode":"upsert"}),
            );
            assert_eq!(final_write["error"], "", "cooperative backend flush remains permitted before shutdown completion: {final_write}");
            advance_plugin_removal(id, RemovalPhase::PrivateData).expect("durable erasure fence");
            let late_write = send(
                serde_json::json!({"id":"late", "op":"write", "storage":storage, "path":"late", "data":"bGF0ZQ==", "mode":"upsert"}),
            );
            assert_eq!(
                late_write["error"], "revoked",
                "existing sessions cannot publish once erasure begins"
            );
            begin_plugin_removal(id).expect("retry cannot reset the fence");
            assert_eq!(
                plugin_removal_phase(id).expect("retained erasure phase"),
                Some(RemovalPhase::PrivateData)
            );
            assert!(
                crate::plugin_bundle::plugin_remove(id.into()).is_empty(),
                "actual cleanup completes the fenced private tree and journal"
            );
            crate::storage::storage_session_revoke(session);
        });
    }

    #[test]
    fn settings_cannot_replace_an_unreadable_removal_journal_with_defaults() {
        with_isolated_datadir(|base| {
            state_crypt::unlock_for_tests();
            begin_plugin_removal("example").expect("durable pending removal");
            let path = base.join("app.bip138");
            let original = fs::read(&path).expect("original journal ciphertext");
            let damaged = b"unreadable encrypted state";
            fs::write(&path, damaged).expect("simulate unreadable journal");
            assert!(
                !app_set_plugin_enabled("example".into(), true).is_empty(),
                "enable reports the actual load failure"
            );
            app_set_active_theme("dark".into());
            app_set_language("fr".into());
            app_set_price_selection("other-provider".into(), "USD".into());
            app_set_fee_preferences(RustFeePreferences {
                mode: 0,
                default_source: String::new(),
                smart_source: String::new(),
                tier: 1,
            });
            app_set_qr_display_levels(QrDisplayLevels {
                density: 1,
                contrast: 1,
                speed: 1,
            });
            app_set_qr_camera_contrast_level(1);
            app_set_last_endpoint(
                BackendServer::Electrum,
                Network::Regtest,
                "localhost:50001".into(),
            );
            assert_eq!(
                fs::read(&path).expect("journal after refused updates"),
                damaged,
                "no settings mutation may overwrite an unreadable removal journal"
            );
            fs::write(&path, original).expect("repair journal fixture");
            assert_eq!(
                plugin_removal_phase("example").expect("recovered removal phase"),
                Some(RemovalPhase::Stopping),
                "original pending phase survives recovery"
            );
        });
    }

    #[test]
    fn plugin_removal_retries_final_save_failure_after_rename_without_false_success() {
        with_isolated_datadir(|_base| {
            state_crypt::unlock_for_tests();
            let id = "post-rename-removal";
            begin_plugin_removal(id).expect("initial checkpoint");
            advance_plugin_removal(id, RemovalPhase::PrivateData).expect("private phase");
            advance_plugin_removal(id, RemovalPhase::Bundle).expect("bundle phase");
            advance_plugin_removal(id, RemovalPhase::Bookkeeping).expect("bookkeeping phase");
            state_crypt::fail_durable_write_at(state_crypt::DurableWriteStage::Renamed);
            assert!(
                finish_plugin_removal(id).is_err(),
                "post-rename fsync failure is reported, not hidden as success"
            );
            assert_eq!(
                plugin_removal_phase(id).expect("state after interrupted final save"),
                None,
                "the complete final state may already have replaced its predecessor"
            );
            begin_plugin_removal(id)
                .expect("actual public retry checkpoints again before uninstall");
            assert!(crate::plugin_bundle::plugin_remove(id.into()).is_empty(), "the real begin/uninstall retry completes even when the previous journal entry was already gone");
            assert_eq!(plugin_removal_phase(id).expect("completed retry"), None);
        });
    }

    #[test]
    fn ordinary_settings_replacements_preserve_durable_removal_checkpoints_on_failure() {
        with_isolated_datadir(|base| {
            state_crypt::unlock_for_tests();
            begin_plugin_removal("example").expect("durable pending removal");
            advance_plugin_removal("example", RemovalPhase::PrivateData)
                .expect("durable erasure phase");
            let before = fs::read(base.join("app.bip138")).expect("durable journal bytes");
            state_crypt::fail_durable_write_at(state_crypt::DurableWriteStage::TempSynced);
            app_set_active_theme("dark".into());
            assert_eq!(
                fs::read(base.join("app.bip138")).expect("journal after failed ordinary update"),
                before,
                "ordinary preferences use the same durable atomic replacement as removal phases"
            );
            assert_eq!(
                plugin_removal_phase("example").expect("retained erasure phase"),
                Some(RemovalPhase::PrivateData)
            );
        });
    }

    #[test]
    fn grant_generation_tracks_actual_publication_when_durable_save_fails() {
        with_isolated_datadir(|_base| {
            state_crypt::unlock_for_tests();
            let id = "grant-durability";
            let hash = "d".repeat(64);
            let access = |state| PluginAccess {
                plugin_id: id.into(),
                hash: hash.clone(),
                account: PluginGrant::Unasked,
                keys: PluginGrant::Unasked,
                state,
            };
            let epoch = app_plugin_access_request(id.into(), hash.clone()).storage_epoch;
            try_set_plugin_access(access(PluginGrant::Allowed), epoch)
                .expect("initial allowed grant");
            let session = crate::storage::storage_session_open(id.into(), hash.clone(), true);
            let before = crate::storage::storage_session_generation(session);
            state_crypt::fail_durable_write_at(state_crypt::DurableWriteStage::TempSynced);
            assert!(
                try_set_plugin_access(access(PluginGrant::Denied), epoch).is_err(),
                "pre-publication save error"
            );
            assert_eq!(
                crate::storage::storage_session_generation(session),
                before,
                "failed unpublished grants do not invalidate an authorized generation"
            );
            state_crypt::fail_durable_write_at(state_crypt::DurableWriteStage::Renamed);
            assert!(
                try_set_plugin_access(access(PluginGrant::Denied), epoch).is_err(),
                "post-publication durability error remains explicit"
            );
            assert!(
                !app_plugin_grants(hash.clone()).state,
                "a complete denied grant may already be published"
            );
            assert!(crate::storage::storage_session_generation(session) > before, "published grant changes always invalidate stale responses even after fsync failure");
            crate::storage::storage_session_revoke(session);
        });
    }

    #[test]
    fn grant_requests_and_saves_fail_on_an_unusable_lock_or_unreadable_state() {
        with_isolated_datadir(|base| {
            state_crypt::unlock_for_tests();
            let id = "grant-failures";
            let hash = "e".repeat(64);
            let allowed = PluginAccess {
                plugin_id: id.into(),
                hash: hash.clone(),
                account: PluginGrant::Allowed,
                keys: PluginGrant::Unasked,
                state: PluginGrant::Unasked,
            };
            save_access(allowed.clone());
            let path = base.join("app.bip138");
            let original = fs::read(&path).expect("original granted state");

            let lock = base.join("app.lock");
            fs::remove_file(&lock).expect("drop the app-state lock file");
            fs::create_dir(&lock).expect("unusable app-state lock fixture");
            let request = app_plugin_access_request(id.into(), hash.clone());
            assert!(!request.is_ok, "an unusable lock refuses the request");
            assert!(
                request.error.starts_with("App-state lock:"),
                "the request reports the lock failure: {}",
                request.error
            );
            assert_eq!(
                request.access.account,
                PluginGrant::Unasked,
                "a refused request carries no stored answer"
            );
            let history = app_plugin_access_history(id.into());
            assert!(!history.is_ok, "an unusable lock refuses the history");
            assert!(
                history.entries.is_empty(),
                "a refused history lists nothing"
            );
            assert!(
                app_set_plugin_access(allowed.clone(), 0).starts_with("App-state lock:"),
                "an unusable lock refuses the save"
            );
            fs::remove_dir(&lock).expect("restore the app-state lock file");

            fs::write(&path, b"unreadable grant state").expect("unreadable state fixture");
            let request = app_plugin_access_request(id.into(), hash.clone());
            assert!(!request.is_ok, "an unreadable state refuses the request");
            assert!(
                request.error.contains("not encrypted"),
                "the request reports the read failure: {}",
                request.error
            );
            assert!(!app_plugin_access_history(id.into()).is_ok);
            let denied = PluginAccess {
                account: PluginGrant::Denied,
                ..allowed
            };
            assert!(
                app_set_plugin_access(denied, 0).contains("not encrypted"),
                "an unreadable state refuses the save"
            );
            assert_eq!(
                fs::read(&path).expect("state after refused grants"),
                b"unreadable grant state",
                "no refused grant replaces the unreadable state"
            );
            fs::write(&path, original).expect("restore the granted state");
            assert!(
                app_plugin_grants(hash).account,
                "the stored grant is intact"
            );
        });
    }

    #[test]
    fn grant_requests_bind_the_epoch_to_the_requested_plugin_and_refuse_locked_state() {
        with_isolated_datadir(|base| {
            let first = "grant-first";
            let second = "grant-second";
            let hash = "same-library-hash";
            save_access(PluginAccess {
                plugin_id: first.into(),
                hash: hash.into(),
                account: PluginGrant::Allowed,
                keys: PluginGrant::Unasked,
                state: PluginGrant::Allowed,
            });
            let held = app_plugin_access_request(second.into(), hash.into());
            assert!(held.is_ok, "checked request: {}", held.error);
            assert_eq!(held.access.plugin_id, second, "answers keep the authenticated request identity even when the library hash is shared");
            assert_eq!(held.access.hash, hash);
            let before = fs::read(base.join("app.bip138")).expect("original ciphertext");
            state_crypt::lock();
            let locked = app_plugin_access_request(second.into(), hash.into());
            assert!(!locked.is_ok && locked.error.contains("locked"));
            let history = app_plugin_access_history(second.into());
            assert!(!history.is_ok && history.error.contains("locked"));
            assert!(
                app_set_plugin_access(held.access.clone(), held.storage_epoch).contains("locked")
            );
            assert_eq!(
                fs::read(base.join("app.bip138")).expect("locked ciphertext"),
                before
            );
            state_crypt::unlock_for_tests();
            begin_plugin_removal(second).expect("second plugin removal starts");
            assert_eq!(crate::plugin_bundle::plugin_remove(second.into()), "");
            assert!(app_set_plugin_access(held.access, held.storage_epoch).contains("removed since"), "the request compares the requested plugin's epoch, not the previous grant owner's epoch");
            assert!(app_plugin_grants(hash.into()).state, "the first plugin's hash-specific grant was not changed by the refused second-plugin request");
        });
    }

    #[test]
    fn grant_saves_need_the_epoch_read_by_their_request_and_no_pending_removal() {
        with_isolated_datadir(|_base| {
            state_crypt::unlock_for_tests();
            let id = "grant-epoch";
            let hash = "c".repeat(64);
            let held = app_plugin_access_request(id.into(), hash.clone());
            assert!(held.is_ok, "request before the removal: {}", held.error);
            assert_eq!(held.storage_epoch, 0, "first lifetime");
            let history = app_plugin_access_history(id.into());
            assert!(
                history.is_ok,
                "history before the removal: {}",
                history.error
            );
            assert_eq!(history.storage_epoch, 0);
            let allow = |request: &PluginAccessRequest| PluginAccess {
                keys: PluginGrant::Allowed,
                ..request.access.clone()
            };

            begin_plugin_removal(id).expect("pending removal");
            let refused = app_plugin_access_request(id.into(), hash.clone());
            assert!(!refused.is_ok, "no request while the removal is pending");
            assert!(
                refused.error.contains("removal is pending"),
                "{}",
                refused.error
            );
            assert!(!app_plugin_access_history(id.into()).is_ok);
            assert!(
                app_set_plugin_access(allow(&held), held.storage_epoch)
                    .contains("removal is pending"),
                "a request held across the removal start cannot save"
            );

            assert_eq!(crate::plugin_bundle::plugin_remove(id.into()), "");
            assert_eq!(plugin_removal_phase(id).expect("finished removal"), None);
            let stale = app_set_plugin_access(allow(&held), held.storage_epoch);
            assert!(
                stale.contains("removed since"),
                "a request held across the whole removal cannot save: {stale}"
            );
            assert!(
                app_set_plugin_access(allow(&held), history.storage_epoch)
                    .contains("removed since"),
                "a Settings view held across the removal cannot save"
            );
            assert!(app_plugin_history(id.into()).is_empty(), "no grant revived");
            assert!(!app_plugin_grants(hash.clone()).keys);

            let fresh = app_plugin_access_request(id.into(), hash.clone());
            assert!(fresh.is_ok, "request after the removal: {}", fresh.error);
            assert_eq!(fresh.storage_epoch, 1, "the removal rotated the epoch");
            assert_eq!(fresh.access.keys, PluginGrant::Unasked);
            assert_eq!(
                app_set_plugin_access(allow(&fresh), fresh.storage_epoch),
                "",
                "a fresh request saves"
            );
            assert!(app_plugin_grants(hash).keys);
        });
    }

    #[test]
    fn load_missing_file_returns_defaults() {
        with_isolated_datadir(|base| {
            assert!(
                !base.join("app.bip138").exists(),
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
    fn fee_preferences_roundtrip_all_modes_and_tiers() {
        with_isolated_datadir(|_base| {
            let legacy: AppState = serde_json::from_str("{}").expect("legacy app state");
            assert_eq!(legacy.fee_preferences.mode, 0, "old state starts Manual");
            assert_eq!(legacy.fee_preferences.tier, 1, "old state starts Medium");
            for mode in 0..=2 {
                for tier in 0..=2 {
                    app_set_fee_preferences(RustFeePreferences {
                        mode,
                        default_source: "default-provider".into(),
                        smart_source: "smart-provider".into(),
                        tier,
                    });
                    let loaded = app_fee_preferences();
                    assert_eq!(loaded.mode, mode, "persist fee mode {mode}");
                    assert_eq!(loaded.tier, tier, "persist smart tier {tier}");
                    assert_eq!(
                        loaded.default_source, "default-provider",
                        "retain default source"
                    );
                    assert_eq!(
                        loaded.smart_source, "smart-provider",
                        "retain independent smart source"
                    );
                }
            }
            app_set_fee_preferences(RustFeePreferences {
                mode: 3,
                ..RustFeePreferences::default()
            });
            assert_eq!(
                app_fee_preferences().mode,
                2,
                "reject invalid mode without overwriting preferences"
            );
            app_set_fee_preferences(RustFeePreferences {
                tier: 3,
                ..RustFeePreferences::default()
            });
            assert_eq!(
                app_fee_preferences().tier,
                2,
                "reject invalid tier without overwriting preferences"
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
            save_access(access("AA", PluginGrant::Allowed, PluginGrant::Unasked));
            save_access(access("bb", PluginGrant::Denied, PluginGrant::Denied));
            let grants = app_plugin_grants("AA".into());
            assert!(
                grants.account && !grants.keys && !grants.state,
                "grant evaluation is hash-specific and unasked is denied"
            );
            assert!(
                !app_plugin_grants("new-library-hash".into()).account,
                "upgraded library does not inherit grants"
            );
            assert_eq!(
                app_plugin_history("example".into()).len(),
                2,
                "plugin history is filtered in Rust"
            );
            assert!(
                app_plugin_history("other-plugin".into()).is_empty(),
                "unrelated plugin history is empty"
            );
            assert_eq!(
                app_plugin_access(),
                vec![
                    access("aa", PluginGrant::Allowed, PluginGrant::Unasked),
                    access("bb", PluginGrant::Denied, PluginGrant::Denied),
                ],
                "each library hash keeps its own answers, the hash stored lowercase"
            );
            save_access(access("aa", PluginGrant::Allowed, PluginGrant::Allowed));
            assert_eq!(
                app_plugin_access()[0],
                access("aa", PluginGrant::Allowed, PluginGrant::Allowed),
                "a new answer replaces the one stored for the same hash"
            );
            let stored = state_crypt::read_state(&base.join("app.bip138"))
                .expect("read app.json")
                .expect("app.json written");
            assert!(
                stored.contains("\"keys\": \"allowed\""),
                "grants are stored by name: {stored}"
            );
            save_access(access("bb", PluginGrant::Unasked, PluginGrant::Unasked));
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
            assert!(
                app_set_plugin_enabled("plugin-a".to_string(), true).is_empty(),
                "enable plugin-a"
            );
            assert!(
                app_set_plugin_enabled("plugin-a".to_string(), true).is_empty(),
                "duplicate enable is idempotent"
            );
            assert!(
                app_set_plugin_enabled("plugin-b".to_string(), true).is_empty(),
                "enable plugin-b"
            );
            assert!(
                app_set_plugin_enabled("plugin-a".to_string(), false).is_empty(),
                "disable plugin-a"
            );
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
            state_crypt::write_state(&base.join("app.bip138"), "{ this is not valid json")
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
                &base.join("app.bip138"),
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
                &base.join("app.bip138"),
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
