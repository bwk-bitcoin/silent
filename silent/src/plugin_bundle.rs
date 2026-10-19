//! External plugins, kept in the app state like everything else (ws-plaude/silent#41).
//!
//! A plugin enters only through `install`, from the folder or the library the user
//! dropped in the app: its sidecar, its library and its translation catalogs are
//! read once and sealed together into one bundle, `plugins/<opaque>.bip138`. Nothing
//! else under `~/.silent/plugins/` is ever read, so a library swapped on disk is not
//! loaded: the host loads the bytes `library` opens, the ones it hashes.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use base64::prelude::BASE64_STANDARD;
use base64::Engine;
use serde::{Deserialize, Serialize};

use crate::config::datadir;
use crate::ffi::{PluginCatalog, PluginInstallResult, PluginLibraryResult};
use crate::state_crypt::{self, SEALED_EXTENSION};

/// Where the sealed bundles live.
pub const PLUGINS_DIR: &str = "plugins";
const BUNDLE_VERSION: u32 = 1;

/// One installed plugin, as sealed.
#[derive(Debug, Clone, Serialize, Deserialize)]
struct Bundle {
    version: u32,
    /// The sidecar, as the plugin ships it: its `id`, `name`, `version`...
    sidecar: serde_json::Map<String, serde_json::Value>,
    /// The library's file name, for its type (`.so`, `.dylib`, `.dll`).
    library_name: String,
    /// The library, base64.
    library: String,
    /// Translation catalogs by file name (`silent_en.lang`).
    #[serde(default)]
    catalogs: BTreeMap<String, String>,
}

/// An installed plugin's library and catalogs, opened.
#[derive(Debug, Clone)]
pub struct Opened {
    pub library_name: String,
    pub library: Vec<u8>,
    pub catalogs: Vec<(String, String)>,
}

fn library_suffix() -> &'static str {
    if cfg!(target_os = "macos") {
        "dylib"
    } else if cfg!(target_os = "windows") {
        "dll"
    } else {
        "so"
    }
}

fn valid_id(id: &str) -> bool {
    !id.is_empty()
        && !id.starts_with('.')
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_'))
}

fn bundle_path(id: &str) -> Result<PathBuf, String> {
    Ok(datadir().join(PLUGINS_DIR).join(format!(
        "{}.{SEALED_EXTENSION}",
        state_crypt::opaque_name("plugin-bundle", id)?
    )))
}

fn read_sidecar(path: &Path) -> Option<serde_json::Map<String, serde_json::Value>> {
    let text = fs::read_to_string(path).ok()?;
    match serde_json::from_str::<serde_json::Value>(&text).ok()? {
        serde_json::Value::Object(map) => Some(map),
        _ => None,
    }
}

fn sidecar_id(sidecar: &serde_json::Map<String, serde_json::Value>) -> Option<String> {
    sidecar
        .get("id")
        .and_then(|id| id.as_str())
        .map(|id| id.trim().to_string())
}

/// The sidecar of the plugin in `dir`: `<dir name>.json` first, else the only JSON
/// file there that names an id.
fn find_sidecar(
    dir: &Path,
) -> Result<(PathBuf, serde_json::Map<String, serde_json::Value>), String> {
    let name = dir.file_name().and_then(|n| n.to_str()).unwrap_or("");
    let named = dir.join(format!("{name}.json"));
    if let Some(sidecar) = read_sidecar(&named) {
        if sidecar_id(&sidecar).is_some() {
            return Ok((named, sidecar));
        }
    }
    let mut found = Vec::new();
    for entry in fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))? {
        let path = entry.map_err(|e| e.to_string())?.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        if let Some(sidecar) = read_sidecar(&path) {
            if sidecar_id(&sidecar).is_some() {
                found.push((path, sidecar));
            }
        }
    }
    match found.len() {
        1 => Ok(found.remove(0)),
        0 => Err(format!("no plugin metadata file in {}", dir.display())),
        _ => Err(format!(
            "several plugin metadata files in {}",
            dir.display()
        )),
    }
}

/// The library the sidecar names, else `lib<id>.<suffix>` (`<id>.dll`), else the only
/// library in `dir`.
fn find_library(
    dir: &Path,
    id: &str,
    sidecar: &serde_json::Map<String, serde_json::Value>,
) -> Result<PathBuf, String> {
    if let Some(name) = sidecar.get("library").and_then(|l| l.as_str()) {
        let path = dir.join(name.trim());
        if path.is_file() {
            return Ok(path);
        }
    }
    let default = if cfg!(target_os = "windows") {
        format!("{id}.dll")
    } else {
        format!("lib{id}.{}", library_suffix())
    };
    if dir.join(&default).is_file() {
        return Ok(dir.join(default));
    }
    let libraries: Vec<PathBuf> = fs::read_dir(dir)
        .map_err(|e| format!("{}: {e}", dir.display()))?
        .flatten()
        .map(|entry| entry.path())
        .filter(|path| path.extension().and_then(|e| e.to_str()) == Some(library_suffix()))
        .collect();
    match libraries.as_slice() {
        [library] => Ok(library.clone()),
        [] => Err(format!("no plugin library in {}", dir.display())),
        _ => Err(format!("several plugin libraries in {}", dir.display())),
    }
}

/// Seals the plugin at `dropped` (its folder, or its library with the sidecar beside
/// it) into the state, replacing an earlier version of the same id. Answers its id.
pub fn install(dropped: &Path) -> Result<String, String> {
    let (dir, dropped_library) = if dropped.is_dir() {
        (dropped.to_path_buf(), None)
    } else if dropped.is_file() {
        let dir = dropped
            .parent()
            .ok_or_else(|| format!("{} has no folder", dropped.display()))?;
        (dir.to_path_buf(), Some(dropped.to_path_buf()))
    } else {
        return Err(format!("{} is not a file or a folder", dropped.display()));
    };
    let (_, sidecar) = find_sidecar(&dir)?;
    let id = sidecar_id(&sidecar).unwrap_or_default();
    if !valid_id(&id) {
        return Err(format!("invalid plugin id '{id}'"));
    }
    let library_path = match dropped_library {
        Some(library) => library,
        None => find_library(&dir, &id, &sidecar)?,
    };
    let library =
        fs::read(&library_path).map_err(|e| format!("{}: {e}", library_path.display()))?;
    let mut catalogs = BTreeMap::new();
    if let Ok(entries) = fs::read_dir(dir.join("i18n")) {
        for path in entries.flatten().map(|entry| entry.path()) {
            let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
            if name.starts_with("silent_") && name.ends_with(".lang") {
                if let Ok(text) = fs::read_to_string(&path) {
                    catalogs.insert(name.to_string(), text);
                }
            }
        }
    }
    let bundle = Bundle {
        version: BUNDLE_VERSION,
        sidecar,
        library_name: library_path
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or_default()
            .to_string(),
        library: BASE64_STANDARD.encode(library),
        catalogs,
    };
    let text = serde_json::to_string(&bundle).map_err(|e| e.to_string())?;
    state_crypt::write_state(&bundle_path(&id)?, &text)?;
    Ok(id)
}

fn read_bundle(path: &Path) -> Result<Bundle, String> {
    let text =
        state_crypt::read_state(path)?.ok_or_else(|| format!("{} is gone", path.display()))?;
    let bundle: Bundle =
        serde_json::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))?;
    if bundle.version > BUNDLE_VERSION {
        return Err(format!("{} was written by a newer Silent", path.display()));
    }
    Ok(bundle)
}

/// Every installed plugin's sidecar, as JSON text. A bundle that does not open is
/// skipped.
pub fn list() -> Vec<String> {
    let dir = datadir().join(PLUGINS_DIR);
    let Ok(entries) = fs::read_dir(&dir) else {
        return Vec::new();
    };
    let mut sidecars = Vec::new();
    for path in entries.flatten().map(|entry| entry.path()) {
        if path.extension().and_then(|e| e.to_str()) != Some(SEALED_EXTENSION) {
            continue;
        }
        match read_bundle(&path) {
            Ok(bundle) => {
                // A bundle under another id's name is not that plugin.
                let id = sidecar_id(&bundle.sidecar).unwrap_or_default();
                if bundle_path(&id).ok().as_deref() != Some(path.as_path()) {
                    log::error!("plugin bundle {} does not match its id", path.display());
                    continue;
                }
                sidecars.push(serde_json::Value::Object(bundle.sidecar).to_string());
            }
            Err(e) => log::error!("plugin bundle: {e}"),
        }
    }
    sidecars.sort();
    sidecars
}

/// The library and catalogs of the plugin `id`.
pub fn open(id: &str) -> Result<Opened, String> {
    if !valid_id(id) {
        return Err(format!("invalid plugin id '{id}'"));
    }
    let bundle = read_bundle(&bundle_path(id)?)?;
    let library = BASE64_STANDARD
        .decode(bundle.library.as_bytes())
        .map_err(|e| format!("plugin '{id}': {e}"))?;
    Ok(Opened {
        library_name: bundle.library_name,
        library,
        catalogs: bundle.catalogs.into_iter().collect(),
    })
}

/// Removes the installed plugin `id`.
pub fn remove(id: &str) -> Result<(), String> {
    let path = bundle_path(id)?;
    match fs::remove_file(&path) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(format!("{}: {e}", path.display())),
    }
}

// ===== Bridge =====

pub fn plugin_install(path: String) -> PluginInstallResult {
    match install(Path::new(&path)) {
        Ok(plugin_id) => PluginInstallResult {
            is_ok: true,
            plugin_id,
            error: String::new(),
        },
        Err(error) => PluginInstallResult {
            is_ok: false,
            plugin_id: String::new(),
            error,
        },
    }
}

pub fn plugin_bundles() -> Vec<String> {
    list()
}

pub fn plugin_library(plugin_id: String) -> PluginLibraryResult {
    match open(&plugin_id) {
        Ok(opened) => PluginLibraryResult {
            is_ok: true,
            library_name: opened.library_name,
            library: opened.library,
            catalogs: opened
                .catalogs
                .into_iter()
                .map(|(name, text)| PluginCatalog { name, text })
                .collect(),
            error: String::new(),
        },
        Err(error) => PluginLibraryResult {
            is_ok: false,
            library_name: String::new(),
            library: Vec::new(),
            catalogs: Vec::new(),
            error,
        },
    }
}

pub fn plugin_remove(plugin_id: String) -> String {
    remove(&plugin_id).err().unwrap_or_default()
}
