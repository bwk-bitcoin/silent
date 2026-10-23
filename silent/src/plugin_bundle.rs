//! External plugins, kept in the app state like everything else (ws-plaude/silent#41).
//!
//! A plugin enters only through `install`, from the bundle the user dropped in the
//! app, whatever its file name (ws-plaude/silent#69). The bundle is a ZIP holding a
//! root `manifest.json`, one library under `payload/` and the translation catalogs
//! under `i18n/` the manifest declares, nothing else. It is read in memory, its
//! library admitted without running it, then sealed into one installed bundle,
//! `plugins/<opaque>.bip138`. Nothing else under `~/.silent/plugins/` is ever read, so
//! a library swapped on disk is not loaded: the host loads the bytes `library`
//! opens, the ones it hashes.

use std::{
    collections::BTreeMap,
    fs,
    io::Read,
    path::{Path, PathBuf},
};

#[cfg(unix)]
use std::os::unix::fs::OpenOptionsExt;

use base64::prelude::BASE64_STANDARD;
use base64::Engine;
use serde::{Deserialize, Serialize};

use crate::{
    app_state::{self, RemovalPhase},
    config::datadir,
    ffi::{
        PluginCatalog, PluginInstallResult, PluginLibraryResult, PluginMetadata,
        PluginMetadataField, PluginMetadataKind,
    },
    plugin_admission::{self, Compatibility},
    plugin_bundle::archive::Archive,
    state_crypt::{self, SEALED_EXTENSION},
};

#[path = "plugin_zip.rs"]
mod archive;

/// Where the sealed bundles live.
pub const PLUGINS_DIR: &str = "plugins";
const BUNDLE_VERSION: u32 = 1;

const FORMAT_VERSION: u32 = 1;
const MANIFEST: &str = "manifest.json";
const PAYLOAD_DIR: &str = "payload/";
const CATALOG_DIR: &str = "i18n/";
const CATALOG_PREFIX: &str = "silent_";
const CATALOG_SUFFIX: &str = ".lang";

const MAX_MANIFEST_BYTES: usize = 64 * 1024;
const MAX_LIBRARY_BYTES: usize = 256 * 1024 * 1024;
const MAX_CATALOG_BYTES: usize = 1024 * 1024;
const MAX_TRANSLATIONS: usize = 64;
const MAX_ENTRIES: usize = 2 + MAX_TRANSLATIONS;
/// The library plus room for the manifest and the catalogs.
const MAX_DECODED_BYTES: u64 = MAX_LIBRARY_BYTES as u64 + 16 * 1024 * 1024;
const MAX_ARCHIVE_BYTES: u64 = MAX_DECODED_BYTES;
const MAX_TEXT_BYTES: usize = 4096;

/// One installed plugin, as sealed.
#[derive(Debug, Clone, Serialize, Deserialize)]
struct Bundle {
    version: u32,
    /// The manifest, as the plugin ships it: its `id`, `name`, `version`...
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

/// The bundle's `manifest.json`. Duplicate and unknown keys are refused at every
/// depth: every object is a typed struct.
#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Manifest {
    format_version: u32,
    id: String,
    name: String,
    version: String,
    description: String,
    author: String,
    capabilities: Vec<String>,
    entry: ManifestEntry,
    /// Archive paths of the catalogs, `i18n/silent_<locale>.lang`.
    translations: Vec<String>,
}

/// How the host runs the plugin: only an in-process library for now, other kinds
/// are refused.
#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
enum ManifestEntry {
    Native {
        /// Archive path of the library, `payload/<file name>`.
        library: String,
        compatibility: ManifestCompatibility,
    },
}

/// `Compatibility` as the manifest declares it, unknown keys refused.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ManifestCompatibility {
    iid: String,
    target_os: String,
    target_arch: String,
    qt_major: u8,
    qt_minor: u8,
    arch_requirements: u8,
}

impl From<ManifestCompatibility> for Compatibility {
    fn from(declared: ManifestCompatibility) -> Self {
        Compatibility {
            iid: declared.iid,
            target_os: declared.target_os,
            target_arch: declared.target_arch,
            qt_major: declared.qt_major,
            qt_minor: declared.qt_minor,
            arch_requirements: declared.arch_requirements,
        }
    }
}

fn library_suffix(target_os: &str) -> Option<&'static str> {
    match target_os {
        "linux" => Some(".so"),
        "windows" => Some(".dll"),
        "macos" => Some(".dylib"),
        _ => None,
    }
}

/// A plugin id, a library file name or a catalog locale.
fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && !name.starts_with('.')
        && name
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_'))
}

fn check_text(field: &str, text: &str) -> Result<(), String> {
    if text.is_empty()
        || text.trim() != text
        || text.len() > MAX_TEXT_BYTES
        || text.chars().any(char::is_control)
    {
        return Err(format!("invalid manifest {field}"));
    }
    Ok(())
}

fn check_unique(field: &str, items: &[String]) -> Result<(), String> {
    for (index, item) in items.iter().enumerate() {
        if items[..index].contains(item) {
            return Err(format!("duplicate manifest {field} '{item}'"));
        }
    }
    Ok(())
}

fn check_compatibility(library: &str, declared: &ManifestCompatibility) -> Result<(), String> {
    check_text("entry IID", &declared.iid)?;
    let suffix = library_suffix(&declared.target_os)
        .ok_or_else(|| format!("unsupported manifest target OS '{}'", declared.target_os))?;
    if !matches!(declared.target_arch.as_str(), "x86_64" | "aarch64" | "x86") {
        return Err(format!(
            "unsupported manifest target architecture '{}'",
            declared.target_arch
        ));
    }
    let file = library.strip_prefix(PAYLOAD_DIR).unwrap_or_default();
    if !valid_name(file) || !file.ends_with(suffix) {
        return Err(format!(
            "manifest library '{library}' is not payload/<file name>{suffix}"
        ));
    }
    Ok(())
}

impl Manifest {
    /// Reads and checks the manifest of `archive`, before anything else is decoded.
    fn read(archive: &Archive) -> Result<Self, String> {
        let entry = archive
            .entry(MANIFEST)
            .ok_or_else(|| format!("no {MANIFEST} at the bundle root"))?;
        let manifest: Manifest = serde_json::from_slice(&entry.decode(MAX_MANIFEST_BYTES)?)
            .map_err(|e| format!("invalid {MANIFEST}: {e}"))?;
        manifest.check()?;
        Ok(manifest)
    }

    fn check(&self) -> Result<(), String> {
        if self.format_version != FORMAT_VERSION {
            return Err(format!(
                "unsupported bundle format version {}, expected {FORMAT_VERSION}",
                self.format_version
            ));
        }
        if !valid_name(&self.id) {
            return Err(format!("invalid plugin id '{}'", self.id));
        }
        for (field, text) in [
            ("name", &self.name),
            ("version", &self.version),
            ("description", &self.description),
            ("author", &self.author),
        ] {
            check_text(field, text)?;
        }
        for capability in &self.capabilities {
            check_text("capability", capability)?;
        }
        check_unique("capability", &self.capabilities)?;
        if self.translations.len() > MAX_TRANSLATIONS {
            return Err(format!(
                "more than {MAX_TRANSLATIONS} manifest translations"
            ));
        }
        for path in &self.translations {
            let locale = path
                .strip_prefix(CATALOG_DIR)
                .and_then(|name| name.strip_prefix(CATALOG_PREFIX))
                .and_then(|name| name.strip_suffix(CATALOG_SUFFIX))
                .unwrap_or_default();
            if !valid_name(locale) {
                return Err(format!(
                    "manifest translation '{path}' is not i18n/silent_<locale>.lang"
                ));
            }
        }
        check_unique("translation", &self.translations)?;
        let ManifestEntry::Native {
            library,
            compatibility,
        } = &self.entry;
        check_compatibility(library, compatibility)
    }

    /// Refuses any archive entry besides the manifest and the entries it declares.
    fn check_undeclared(&self, archive: &Archive) -> Result<(), String> {
        let ManifestEntry::Native { library, .. } = &self.entry;
        for entry in &archive.entries {
            let declared = entry.name == MANIFEST
                || entry.name == library
                || self.translations.iter().any(|path| path == entry.name);
            if !declared {
                return Err(format!("undeclared bundle entry '{}'", entry.name));
            }
        }
        Ok(())
    }

    /// Decodes the declared entries of `archive` and admits the library for `host`
    /// against the declared compatibility, without running it.
    fn bundle(&self, archive: &Archive, host: &Compatibility) -> Result<Bundle, String> {
        self.check_undeclared(archive)?;
        let ManifestEntry::Native {
            library,
            compatibility,
        } = &self.entry;
        let library_name = library.trim_start_matches(PAYLOAD_DIR).to_string();
        let library = archive.decode(library, MAX_LIBRARY_BYTES)?;
        let mut catalogs = BTreeMap::new();
        for path in &self.translations {
            let text = String::from_utf8(archive.decode(path, MAX_CATALOG_BYTES)?)
                .map_err(|_| format!("'{path}' is not UTF-8 text"))?;
            catalogs.insert(path.trim_start_matches(CATALOG_DIR).to_string(), text);
        }
        plugin_admission::inspect(&library, host, Some(&compatibility.clone().into()))?;
        let sidecar = match serde_json::to_value(self).map_err(|e| e.to_string())? {
            serde_json::Value::Object(sidecar) => sidecar,
            _ => return Err("manifest is not a JSON object".into()),
        };
        Ok(Bundle {
            version: BUNDLE_VERSION,
            sidecar,
            library_name,
            library: BASE64_STANDARD.encode(library),
            catalogs,
        })
    }
}

fn bundle_path(id: &str) -> Result<PathBuf, String> {
    Ok(datadir().join(PLUGINS_DIR).join(format!(
        "{}.{SEALED_EXTENSION}",
        state_crypt::opaque_name("plugin-bundle", id)?
    )))
}

fn sidecar_id(sidecar: &serde_json::Map<String, serde_json::Value>) -> Option<String> {
    sidecar
        .get("id")
        .and_then(|id| id.as_str())
        .map(|id| id.trim().to_string())
}

/// The bytes of the file at `path`, at most `MAX_ARCHIVE_BYTES`.
fn read_archive(path: &Path) -> Result<Vec<u8>, String> {
    let at = |e: std::io::Error| format!("{}: {e}", path.display());
    let mut options = fs::OpenOptions::new();
    options.read(true);
    // Opening a FIFO blocks until a writer comes: never wait, check the opened handle.
    #[cfg(unix)]
    options.custom_flags(rustix::fs::OFlags::NONBLOCK.bits() as i32);
    let file = options.open(path).map_err(at)?;
    let metadata = file.metadata().map_err(at)?;
    if !metadata.is_file() {
        return Err(format!("{} is not a plugin bundle file", path.display()));
    }
    let too_large = || {
        format!(
            "{} exceeds the {MAX_ARCHIVE_BYTES} byte bundle limit",
            path.display()
        )
    };
    if metadata.len() > MAX_ARCHIVE_BYTES {
        return Err(too_large());
    }
    // A file growing while read is still cut at the limit.
    let mut bytes = Vec::new();
    file.take(MAX_ARCHIVE_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(at)?;
    if bytes.len() as u64 > MAX_ARCHIVE_BYTES {
        return Err(too_large());
    }
    Ok(bytes)
}

/// Seals the plugin bundle at `path` into the state once its library is admitted for
/// `host`, replacing an earlier version of the same id. Answers its id. Nothing is
/// written when any check fails.
pub fn install(path: &Path, host: &Compatibility) -> Result<String, String> {
    let bytes = read_archive(path)?;
    let in_bundle = |e: String| format!("{}: {e}", path.display());
    let archive = Archive::parse(&bytes, MAX_ENTRIES).map_err(in_bundle)?;
    let declared: u64 = archive.entries.iter().map(|entry| entry.size).sum();
    if declared > MAX_DECODED_BYTES {
        return Err(in_bundle(format!(
            "bundle declares {declared} decoded bytes, over its {MAX_DECODED_BYTES} byte limit"
        )));
    }
    let manifest = Manifest::read(&archive).map_err(in_bundle)?;
    let bundle = manifest.bundle(&archive, host).map_err(in_bundle)?;
    let text = serde_json::to_string(&bundle).map_err(|e| e.to_string())?;
    let _publication = state_crypt::publication_guard();
    let _state = crate::storage::app_state_lock(&datadir())?;
    if crate::app_state::plugin_removal_pending(&manifest.id)? {
        return Err("plugin removal must be completed before reinstalling".into());
    }
    fs::create_dir_all(datadir().join(PLUGINS_DIR)).map_err(|error| error.to_string())?;
    state_crypt::write_state_durable(&bundle_path(&manifest.id)?, &text)?;
    Ok(manifest.id)
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
    let mut sidecars: Vec<_> = sidecars()
        .into_iter()
        .map(|sidecar| serde_json::Value::Object(sidecar).to_string())
        .collect();
    sidecars.sort();
    sidecars
}

fn sidecars() -> Vec<serde_json::Map<String, serde_json::Value>> {
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
                sidecars.push(bundle.sidecar);
            }
            Err(e) => log::error!("plugin bundle: {e}"),
        }
    }
    sidecars
}

pub fn plugin_metadata() -> Vec<PluginMetadata> {
    let mut result = Vec::new();
    for sidecar in sidecars() {
        let Some(id) = sidecar_id(&sidecar) else {
            continue;
        };
        let mut fields = Vec::new();
        for (key, value) in sidecar {
            if key == "id" {
                continue;
            }
            let mut field = PluginMetadataField {
                key,
                ..Default::default()
            };
            match value {
                serde_json::Value::String(value) => {
                    field.kind = PluginMetadataKind::Text;
                    field.text = value;
                }
                serde_json::Value::Number(value) => {
                    let Some(number) = value.as_f64() else {
                        continue;
                    };
                    field.kind = PluginMetadataKind::Number;
                    field.number = number;
                }
                serde_json::Value::Bool(value) => {
                    field.kind = PluginMetadataKind::Flag;
                    field.flag = value;
                }
                serde_json::Value::Array(values) => {
                    field.kind = PluginMetadataKind::Strings;
                    field.items = values
                        .into_iter()
                        .filter_map(|v| v.as_str().map(str::to_owned))
                        .collect();
                }
                _ => continue,
            }
            fields.push(field);
        }
        result.push(PluginMetadata { id, fields });
    }
    result.sort_by(|a, b| a.id.cmp(&b.id));
    result
}

/// The library and catalogs of the plugin `id`.
pub fn open(id: &str) -> Result<Opened, String> {
    if !valid_name(id) {
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
    let _publication = state_crypt::publication_guard();
    let _state = crate::storage::app_state_lock(&datadir())?;
    remove_bundle(id)
}

fn remove_bundle(id: &str) -> Result<(), String> {
    let path = bundle_path(id)?;
    let temporary = path.with_extension("tmp");
    for entry in [&temporary, &path] {
        match fs::remove_file(entry) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => return Err(format!("{}: {e}", entry.display())),
        }
    }
    #[cfg(unix)]
    if let Some(parent) = path.parent() {
        match fs::File::open(parent) {
            Ok(directory) => directory
                .sync_all()
                .map_err(|error| format!("Bundle deletion durability failed: {error}"))?,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(format!("Bundle deletion durability failed: {error}")),
        }
    }
    Ok(())
}

// ===== Bridge =====

pub fn plugin_install(
    path: String,
    expected_iid: String,
    qt_major: u8,
    qt_minor: u8,
    arch_requirements: u8,
) -> PluginInstallResult {
    let host = Compatibility {
        iid: expected_iid,
        target_os: std::env::consts::OS.into(),
        target_arch: std::env::consts::ARCH.into(),
        qt_major,
        qt_minor,
        arch_requirements,
    };
    match install(Path::new(&path), &host) {
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
    uninstall(&plugin_id).err().unwrap_or_default()
}

fn uninstall(id: &str) -> Result<(), String> {
    crate::storage::removal_transaction(|| uninstall_locked(id))
}

fn uninstall_locked(id: &str) -> Result<(), String> {
    let phase = app_state::plugin_removal_phase(id)?.ok_or("plugin removal was not prepared")?;
    if phase <= RemovalPhase::PrivateData {
        app_state::advance_plugin_removal_locked(id, RemovalPhase::PrivateData).map_err(
            |error| {
                format!(
                "Could not persist private-data cleanup phase; data and bundle retained: {error}"
            )
            },
        )?;
        crate::storage::purge_plugin_locked(id).map_err(|error| {
            format!("Private data cleanup failed; plugin remains disabled: {error}")
        })?;
    }
    if phase <= RemovalPhase::Bundle {
        app_state::advance_plugin_removal_locked(id, RemovalPhase::Bundle).map_err(|error| {
            format!("Private data cleared; could not persist bundle-cleanup phase: {error}")
        })?;
        remove_bundle(id)
            .map_err(|error| format!("Private data cleared; bundle removal failed: {error}"))?;
    }
    app_state::advance_plugin_removal_locked(id, RemovalPhase::Bookkeeping).map_err(|error| {
        format!("Bundle and private data removed; could not persist bookkeeping phase: {error}")
    })?;
    app_state::finish_plugin_removal_locked(id).map_err(|error| {
        format!("Bundle and private data removed; bookkeeping save failed: {error}")
    })
}
