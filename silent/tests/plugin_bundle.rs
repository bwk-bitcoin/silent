//! A plugin enters the state only as a strict ZIP bundle, read in memory and admitted
//! without running its library, then listed and opened from the sealed store only
//! (ws-plaude/silent#41, ws-plaude/silent#69).

use std::{
    fs,
    io::Write,
    ops::Deref,
    path::{Path, PathBuf},
    sync::{mpsc, Mutex, MutexGuard, OnceLock},
    time::Duration,
};

use flate2::{write::DeflateEncoder, Compression};
use serde_json::{json, Value};
use silent::{
    plugin_admission::{self, Compatibility},
    plugin_bundle, set_datadir, state_crypt, PluginMetadataKind,
};

const IID: &str = "dev.silent.IPlugin/12.0";
const MAX_ENTRIES: usize = 66;
const MAX_ARCHIVE_BYTES: u64 = (256 + 16) * 1024 * 1024;
const UNIX_REGULAR: u32 = 0o100_644;
const UNIX_LINK: u32 = 0o120_777;
const UNIX_DIRECTORY: u32 = 0o040_755;
const HOST_DOS: u16 = 0;
const HOST_UNIX: u16 = 3;
const HOST_NTFS: u16 = 10;
const FAKE_LIBRARY: &[u8] = b"\x7fELF not a library";

/// One archive entry, written as a well-formed record unless a field says otherwise.
#[derive(Clone)]
struct Item {
    name: String,
    data: Vec<u8>,
    deflate: bool,
    descriptor: bool,
    flags: u16,
    method: Option<u16>,
    /// The decoded size the headers declare, if not the real one.
    size: Option<u32>,
    crc: Option<u32>,
    local_name: Option<String>,
    /// The creating host in the high byte, then the ZIP version.
    made_by: u16,
    attributes: u32,
    extra: Vec<u8>,
    /// Bytes after the compressed stream, inside the entry data.
    trailing: Vec<u8>,
}

impl Item {
    fn new(name: &str, data: &[u8]) -> Self {
        Item {
            name: name.into(),
            data: data.to_vec(),
            deflate: false,
            descriptor: false,
            flags: 0,
            method: None,
            size: None,
            crc: None,
            local_name: None,
            made_by: (HOST_UNIX << 8) | 20,
            attributes: UNIX_REGULAR << 16,
            extra: Vec::new(),
            trailing: Vec::new(),
        }
    }

    fn deflated(mut self) -> Self {
        self.deflate = true;
        self
    }

    fn compressed(&self) -> Vec<u8> {
        let mut data = if self.deflate {
            let mut encoder = DeflateEncoder::new(Vec::new(), Compression::default());
            encoder.write_all(&self.data).expect("deflate fixture");
            encoder.finish().expect("deflate fixture")
        } else {
            self.data.clone()
        };
        data.extend(&self.trailing);
        data
    }
}

/// Writes the ZIP of `items`, after `prefix` and with `comment`.
fn zip_with(items: &[Item], prefix: &[u8], comment: &[u8]) -> Vec<u8> {
    let mut out = prefix.to_vec();
    let mut central = Vec::new();
    for item in items {
        let data = item.compressed();
        let method = item.method.unwrap_or(if item.deflate { 8 } else { 0 });
        let flags = item.flags | if item.descriptor { 1 << 3 } else { 0 };
        let crc = item.crc.unwrap_or_else(|| crc32fast::hash(&item.data));
        let size = item.size.unwrap_or(item.data.len() as u32);
        let compressed = data.len() as u32;
        let offset = out.len() as u32;
        let local_name = item.local_name.as_deref().unwrap_or(&item.name);
        let (local_crc, local_compressed, local_size) = if item.descriptor {
            (0, 0, 0)
        } else {
            (crc, compressed, size)
        };
        out.extend(0x0403_4b50_u32.to_le_bytes());
        for field in [20, flags, method, 0, 0x21] {
            out.extend(field.to_le_bytes());
        }
        for field in [local_crc, local_compressed, local_size] {
            out.extend(field.to_le_bytes());
        }
        out.extend((local_name.len() as u16).to_le_bytes());
        out.extend((item.extra.len() as u16).to_le_bytes());
        out.extend(local_name.as_bytes());
        out.extend(&item.extra);
        out.extend(&data);
        if item.descriptor {
            for field in [0x0807_4b50, crc, compressed, size] {
                out.extend(field.to_le_bytes());
            }
        }
        central.extend(0x0201_4b50_u32.to_le_bytes());
        for field in [item.made_by, 20, flags, method, 0, 0x21] {
            central.extend(field.to_le_bytes());
        }
        for field in [crc, compressed, size] {
            central.extend(field.to_le_bytes());
        }
        for field in [item.name.len() as u16, item.extra.len() as u16, 0, 0, 0] {
            central.extend(field.to_le_bytes());
        }
        central.extend(item.attributes.to_le_bytes());
        central.extend(offset.to_le_bytes());
        central.extend(item.name.as_bytes());
        central.extend(&item.extra);
    }
    let directory = out.len() as u32;
    out.extend(&central);
    out.extend(0x0605_4b50_u32.to_le_bytes());
    let count = items.len() as u16;
    for field in [0, 0, count, count] {
        out.extend(field.to_le_bytes());
    }
    out.extend((central.len() as u32).to_le_bytes());
    out.extend(directory.to_le_bytes());
    out.extend((comment.len() as u16).to_le_bytes());
    out.extend(comment);
    out
}

fn zip(items: &[Item]) -> Vec<u8> {
    zip_with(items, &[], &[])
}

fn host() -> Compatibility {
    Compatibility {
        iid: IID.into(),
        target_os: std::env::consts::OS.into(),
        target_arch: std::env::consts::ARCH.into(),
        qt_major: 6,
        qt_minor: u8::MAX,
        arch_requirements: 4,
    }
}

fn library_file() -> &'static str {
    match std::env::consts::OS {
        "windows" => "example.dll",
        "macos" => "libexample.dylib",
        _ => "libexample.so",
    }
}

fn manifest(compatibility: &Compatibility) -> Value {
    json!({
        "format_version": 1,
        "id": "example",
        "name": "Example Plugin",
        "version": "0.2.0",
        "description": "An example plugin",
        "author": "Silent",
        "capabilities": ["feed", "settings"],
        "entry": {
            "kind": "native",
            "library": format!("payload/{}", library_file()),
            "compatibility": compatibility,
        },
        "translations": ["i18n/silent_en.lang", "i18n/silent_fr.lang"],
    })
}

/// The manifest, the library and the catalogs `manifest` declares.
fn items(manifest: &Value, library: &[u8]) -> Vec<Item> {
    vec![
        Item::new("manifest.json", manifest.to_string().as_bytes()).deflated(),
        Item::new(&format!("payload/{}", library_file()), library).deflated(),
        Item::new("i18n/silent_en.lang", b"hello => \"Hello\"\n"),
        Item {
            descriptor: true,
            ..Item::new("i18n/silent_fr.lang", b"hello => \"Bonjour\"\n").deflated()
        },
    ]
}

fn write(dir: &Path, name: &str, bytes: &[u8]) -> PathBuf {
    let path = dir.join(name);
    fs::write(&path, bytes).expect("write bundle fixture");
    path
}

fn refused(path: &Path, fragment: &str) {
    let error = plugin_bundle::install(path, &host()).expect_err(fragment);
    assert!(
        error.contains(fragment),
        "expected '{fragment}', got '{error}'"
    );
}

fn refused_bytes(dir: &Path, bytes: &[u8], fragment: &str) {
    refused(&write(dir, "bundle", bytes), fragment);
}

/// The data directory and the unlock key are process wide: store tests share one
/// directory, one at a time, each removing it when done.
struct Store(MutexGuard<'static, PathBuf>);

impl Deref for Store {
    type Target = Path;

    fn deref(&self) -> &Path {
        &self.0
    }
}

impl Drop for Store {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&*self.0);
    }
}

fn store() -> Store {
    static STORE: OnceLock<Mutex<PathBuf>> = OnceLock::new();
    let guard = STORE
        .get_or_init(|| {
            let dir =
                std::env::temp_dir().join(format!("silent-plugin-bundle-{}", std::process::id()));
            set_datadir(dir.clone());
            state_crypt::unlock_for_tests();
            Mutex::new(dir)
        })
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    Store(guard)
}

fn sealed_files(state: &Path) -> Vec<(String, Vec<u8>)> {
    let Ok(entries) = fs::read_dir(state.join(plugin_bundle::PLUGINS_DIR)) else {
        return Vec::new();
    };
    let mut files: Vec<_> = entries
        .flatten()
        .map(|entry| {
            (
                entry.file_name().to_string_lossy().to_string(),
                fs::read(entry.path()).expect("read sealed bundle"),
            )
        })
        .collect();
    files.sort();
    files
}

#[test]
fn loose_files_folders_and_malformed_archives_are_refused() {
    let dir = temp_dir::TempDir::new().expect("bundle dir");
    let dir = dir.path();
    let valid = items(&manifest(&host()), FAKE_LIBRARY);

    // Loose library, standalone manifest, folder: not bundles.
    refused_bytes(dir, FAKE_LIBRARY, "not a ZIP archive");
    refused_bytes(
        dir,
        manifest(&host()).to_string().as_bytes(),
        "not a ZIP archive",
    );
    refused_bytes(dir, b"", "not a ZIP archive");
    let folder = dir.join("example");
    fs::create_dir_all(&folder).unwrap();
    write(
        &folder,
        "example.json",
        manifest(&host()).to_string().as_bytes(),
    );
    write(&folder, library_file(), FAKE_LIBRARY);
    refused(&folder, "is not a plugin bundle file");

    // Every byte belongs to a record.
    refused_bytes(dir, &zip_with(&valid, b"", b"comment"), "trailing comment");
    refused_bytes(dir, &zip_with(&valid, b"prefix", b""), "gaps or overlap");
    let mut trailing = zip(&valid);
    trailing.extend(b"tail");
    refused_bytes(dir, &trailing, "not a ZIP archive");

    let with = |item: Item| {
        let mut items = valid.clone();
        items.push(item);
        zip(&items)
    };
    let replaced = |index: usize, item: Item| {
        let mut items = valid.clone();
        items[index] = item;
        zip(&items)
    };
    for (bytes, fragment) in [
        (
            with(Item::new("manifest.json", b"{}")),
            "duplicate archive entry 'manifest.json'",
        ),
        (
            with(Item::new("MANIFEST.json", b"{}")),
            "duplicate archive entry 'MANIFEST.json'",
        ),
        (
            with(Item::new("../escape.so", b"x")),
            "unsafe archive entry name '../escape.so'",
        ),
        (
            with(Item::new("/etc/escape", b"x")),
            "unsafe archive entry name '/etc/escape'",
        ),
        (
            with(Item::new("payload\\escape.so", b"x")),
            "unsafe archive entry name",
        ),
        (
            with(Item::new("payload/./x.so", b"x")),
            "unsafe archive entry name",
        ),
        (
            with(Item::new("payload//x.so", b"x")),
            "unsafe archive entry name",
        ),
        (
            with(Item::new("payload/", b"")),
            "archive directory entry 'payload/'",
        ),
        (
            with(Item {
                attributes: UNIX_LINK << 16,
                ..Item::new("payload/link.so", b"/etc/passwd")
            }),
            "archive entry 'payload/link.so' is not a regular file",
        ),
        (
            with(Item {
                attributes: 0x10,
                ..Item::new("i18n", b"")
            }),
            "archive entry 'i18n' is not a regular file",
        ),
        (
            with(Item {
                made_by: HOST_DOS << 8,
                attributes: UNIX_LINK << 16,
                ..Item::new("payload/dos-link.so", b"/etc/passwd")
            }),
            "archive entry 'payload/dos-link.so' is not a regular file",
        ),
        (
            with(Item {
                made_by: HOST_NTFS << 8,
                attributes: UNIX_DIRECTORY << 16,
                ..Item::new("payload/ntfs-dir", b"")
            }),
            "archive entry 'payload/ntfs-dir' is not a regular file",
        ),
        (
            with(Item::new("README", b"x")),
            "undeclared bundle entry 'README'",
        ),
        (
            with(Item::new("payload/other.so", b"x")),
            "undeclared bundle entry 'payload/other.so'",
        ),
        (
            with(Item::new("i18n/silent_de.lang", b"x")),
            "undeclared bundle entry",
        ),
        (
            with(Item {
                flags: 1,
                ..Item::new("x", b"x")
            }),
            "is encrypted or uses unsupported flags",
        ),
        (
            with(Item {
                method: Some(12),
                ..Item::new("x", b"x")
            }),
            "unsupported compression method 12",
        ),
        (
            with(Item {
                extra: vec![1, 0, 0, 0],
                ..Item::new("x", b"x")
            }),
            "ZIP64 and encrypted archive entries are not supported",
        ),
        (
            with(Item {
                extra: vec![9, 0, 8, 0],
                ..Item::new("x", b"x")
            }),
            "truncated archive record",
        ),
        (
            with(Item {
                local_name: Some("y".into()),
                ..Item::new("x", b"x")
            }),
            "local and central headers disagree",
        ),
        (
            replaced(
                2,
                Item {
                    crc: Some(0),
                    ..valid[2].clone()
                },
            ),
            "'i18n/silent_en.lang' fails its CRC check",
        ),
        (
            replaced(
                2,
                Item {
                    size: Some(3),
                    ..valid[2].clone()
                },
            ),
            "stored archive entry 'i18n/silent_en.lang' has inconsistent sizes",
        ),
        (
            replaced(
                3,
                Item {
                    size: Some(3),
                    ..valid[3].clone()
                },
            ),
            "is truncated or larger than its header",
        ),
        (
            replaced(
                3,
                Item {
                    size: Some(100),
                    ..valid[3].clone()
                },
            ),
            "'i18n/silent_fr.lang' size disagrees with its header",
        ),
        (
            replaced(
                3,
                Item {
                    trailing: b"hidden".to_vec(),
                    ..valid[3].clone()
                },
            ),
            "'i18n/silent_fr.lang' has data after its compressed stream",
        ),
        (
            replaced(2, Item::new("i18n/silent_en.lang", b"\xff\xfe")),
            "'i18n/silent_en.lang' is not UTF-8 text",
        ),
        (zip(&valid[..1]), "missing archive entry 'payload/"),
        (
            zip(&[valid[0].clone(), valid[1].clone(), valid[3].clone()]),
            "missing archive entry 'i18n/silent_en.lang'",
        ),
        (zip(&valid[1..]), "no manifest.json at the bundle root"),
        (
            replaced(0, Item::new("plugin/manifest.json", &valid[0].data)),
            "no manifest.json at the bundle root",
        ),
    ] {
        refused_bytes(dir, &bytes, fragment);
    }

    // Nothing is refused for its file name: valid structure reaches library admission.
    refused(
        &write(dir, "any.name.at-all", &zip(&valid)),
        "plugin admission",
    );
    refused(
        &write(dir, "extensionless", &zip(&valid)),
        "plugin admission",
    );

    // Entries inflating over many chunks, with and without a data descriptor, decode.
    let large: Vec<u8> = (0..300_000_u32)
        .flat_map(|index| (index % 251).to_string().into_bytes())
        .collect();
    let mut items = valid.clone();
    items[2] = Item::new("i18n/silent_en.lang", &large).deflated();
    items[3].data = large.clone();
    refused_bytes(dir, &zip(&items), "plugin admission");
}

#[test]
fn resource_limits_hold_before_and_while_decoding() {
    let dir = temp_dir::TempDir::new().expect("bundle dir");
    let dir = dir.path();
    let valid = items(&manifest(&host()), FAKE_LIBRARY);

    // Entry count.
    let many: Vec<_> = (0..=MAX_ENTRIES)
        .map(|index| Item::new(&format!("i18n/silent_l{index}.lang"), b""))
        .collect();
    refused_bytes(
        dir,
        &zip(&many),
        "archive holds 67 entries, expected 1 to 66",
    );

    // Per entry declared size, before decoding.
    let mut items = valid.clone();
    items[0].size = Some(64 * 1024 + 1);
    refused_bytes(
        dir,
        &zip(&items),
        "'manifest.json' exceeds its 65536 byte limit",
    );
    let mut items = valid.clone();
    items[2] = Item::new("i18n/silent_en.lang", &vec![b'a'; 1024 * 1024 + 1]).deflated();
    refused_bytes(
        dir,
        &zip(&items),
        "'i18n/silent_en.lang' exceeds its 1048576 byte limit",
    );

    // Total declared size, before decoding anything.
    let mut items = valid.clone();
    items[1].size = Some(u32::MAX);
    refused_bytes(dir, &zip(&items), "over its 285212672 byte limit");

    // A bomb declaring little: decoding stops one byte past the declared size.
    let bomb = Item {
        size: Some(2),
        ..Item::new("manifest.json", &vec![0; 16 * 1024 * 1024]).deflated()
    };
    assert!(bomb.compressed().len() < 64 * 1024);
    let mut items = valid.clone();
    items[0] = bomb;
    refused_bytes(
        dir,
        &zip(&items),
        "'manifest.json' is truncated or larger than its header",
    );

    // A library declaring the largest size and holding little: refused without
    // committing memory for the declared size.
    let mut items = valid.clone();
    items[1].size = Some(256 * 1024 * 1024);
    refused_bytes(
        dir,
        &zip(&items),
        "'payload/libexample.so' size disagrees with its header",
    );
    #[cfg(target_os = "linux")]
    {
        let status = fs::read_to_string("/proc/self/status").expect("process status");
        let peak_kib: u64 = status
            .lines()
            .find_map(|line| line.strip_prefix("VmHWM:"))
            .and_then(|value| value.trim().trim_end_matches("kB").trim().parse().ok())
            .expect("peak resident size");
        assert!(
            peak_kib < 128 * 1024,
            "decoding committed {peak_kib} KiB for a declared size"
        );
    }

    // Input bytes, whatever the content: refused from the file size, before reading.
    let large = dir.join("large");
    fs::File::create(&large)
        .and_then(|file| file.set_len(MAX_ARCHIVE_BYTES + 1))
        .expect("sparse bundle fixture");
    refused(&large, "exceeds the 285212672 byte bundle limit");
}

#[test]
fn the_manifest_schema_is_strict() {
    let dir = temp_dir::TempDir::new().expect("bundle dir");
    let dir = dir.path();
    let bundle = |text: &str| {
        let mut items = items(&manifest(&host()), FAKE_LIBRARY);
        items[0] = Item::new("manifest.json", text.as_bytes());
        zip(&items)
    };
    let edited = |edit: &dyn Fn(&mut Value)| {
        let mut manifest = manifest(&host());
        edit(&mut manifest);
        bundle(&manifest.to_string())
    };
    let text = manifest(&host()).to_string();
    let library = format!("\"library\":\"payload/{}\"", library_file());
    let os = format!("\"target_os\":\"{}\"", std::env::consts::OS);

    for (bytes, fragment) in [
        (
            bundle(&text.replacen("{", "{\"id\":\"other\",", 1)),
            "duplicate field `id`",
        ),
        (
            bundle(&text.replacen(&library, &format!("{library},{library}"), 1)),
            "duplicate field `library`",
        ),
        (
            bundle(&text.replacen(&os, &format!("{os},{os}"), 1)),
            "duplicate field `target_os`",
        ),
        (
            bundle(&text.replacen(
                "\"kind\":\"native\"",
                "\"kind\":\"native\",\"kind\":\"native\"",
                1,
            )),
            "duplicate field `kind`",
        ),
        (bundle(&format!("{text} {{}}")), "trailing characters"),
        (bundle(&format!("\u{feff}{text}")), "invalid manifest.json"),
        (edited(&|m| m["extra"] = json!(1)), "unknown field `extra`"),
        (
            edited(&|m| m["entry"]["sha256"] = json!("00")),
            "unknown field `sha256`",
        ),
        (
            edited(&|m| m["entry"]["compatibility"]["abi"] = json!(8)),
            "unknown field `abi`",
        ),
        (
            edited(&|m| {
                m.as_object_mut().unwrap().remove("author");
            }),
            "missing field `author`",
        ),
        (
            edited(&|m| {
                m.as_object_mut().unwrap().remove("translations");
            }),
            "missing field `translations`",
        ),
        (
            edited(&|m| m["entry"]["kind"] = json!("process")),
            "unknown variant `process`",
        ),
        (
            edited(&|m| m["entry"]["compatibility"]["qt_major"] = json!(300)),
            "invalid manifest.json",
        ),
        (
            edited(&|m| m["version"] = json!(2)),
            "invalid manifest.json",
        ),
        (
            edited(&|m| m["format_version"] = json!(2)),
            "unsupported bundle format version 2, expected 1",
        ),
        (
            edited(&|m| m["id"] = json!("../escape")),
            "invalid plugin id '../escape'",
        ),
        (
            edited(&|m| m["id"] = json!(".hidden")),
            "invalid plugin id '.hidden'",
        ),
        (edited(&|m| m["id"] = json!("")), "invalid plugin id ''"),
        (edited(&|m| m["name"] = json!("")), "invalid manifest name"),
        (
            edited(&|m| m["name"] = json!(" Example")),
            "invalid manifest name",
        ),
        (
            edited(&|m| m["description"] = json!("line\nbreak")),
            "invalid manifest description",
        ),
        (
            edited(&|m| m["author"] = json!("a".repeat(4097))),
            "invalid manifest author",
        ),
        (
            edited(&|m| m["version"] = json!("")),
            "invalid manifest version",
        ),
        (
            edited(&|m| m["capabilities"] = json!(["feed", "feed"])),
            "duplicate manifest capability 'feed'",
        ),
        (
            edited(&|m| m["capabilities"] = json!([""])),
            "invalid manifest capability",
        ),
        (
            edited(&|m| m["translations"] = json!(["i18n/fr.lang"])),
            "'i18n/fr.lang' is not i18n/silent_<locale>.lang",
        ),
        (
            edited(&|m| m["translations"] = json!(["i18n/sub/silent_fr.lang"])),
            "is not i18n/silent_<locale>.lang",
        ),
        (
            edited(&|m| m["translations"] = json!(["silent_fr.lang"])),
            "is not i18n/silent_<locale>.lang",
        ),
        (
            edited(&|m| m["translations"] = json!(["i18n/silent_en.lang", "i18n/silent_en.lang"])),
            "duplicate manifest translation",
        ),
        (
            edited(&|m| m["entry"]["library"] = json!(library_file())),
            "is not payload/<file name>",
        ),
        (
            edited(&|m| m["entry"]["library"] = json!("payload/../x.so")),
            "is not payload/<file name>",
        ),
        (
            edited(&|m| m["entry"]["library"] = json!("payload/sub/x.so")),
            "is not payload/<file name>",
        ),
        (
            edited(&|m| m["entry"]["compatibility"]["target_os"] = json!("beos")),
            "unsupported manifest target OS 'beos'",
        ),
        (
            edited(&|m| m["entry"]["compatibility"]["target_arch"] = json!("mips")),
            "unsupported manifest target architecture 'mips'",
        ),
        (
            edited(&|m| m["entry"]["compatibility"]["iid"] = json!("")),
            "invalid manifest entry IID",
        ),
    ] {
        refused_bytes(dir, &bytes, fragment);
    }

    // A library for another OS than it declares.
    let foreign = match std::env::consts::OS {
        "windows" => "payload/libexample.so",
        _ => "payload/example.dll",
    };
    refused_bytes(
        dir,
        &edited(&|m| m["entry"]["library"] = json!(foreign)),
        "is not payload/<file name>",
    );

    // Capabilities and translations may be empty: such a bundle reaches admission.
    let mut minimal = manifest(&host());
    minimal["capabilities"] = json!([]);
    minimal["translations"] = json!([]);
    let mut minimal_items = items(&minimal, FAKE_LIBRARY);
    minimal_items.truncate(2);
    refused_bytes(
        dir,
        &zip(&minimal_items),
        "plugin admission (expected dev.silent.IPlugin/12.0)",
    );
}

#[test]
fn a_refused_bundle_leaves_installed_plugins_untouched() {
    let state = store();
    let dir = temp_dir::TempDir::new().expect("bundle dir");

    // A bundle sealed by an earlier Silent, from a folder sidecar.
    let id = "legacy";
    let old = json!({
        "version": 1,
        "sidecar": {"id": id, "name": "Legacy Plugin", "abi": 4, "enabled": true, "modules": ["a", "b"]},
        "library_name": "liblegacy.so",
        "library": "f0VMRiBsZWdhY3k=",
        "catalogs": {"silent_en.lang": "hello => \"Hello\"\n"},
    });
    let path = state.join(plugin_bundle::PLUGINS_DIR).join(format!(
        "{}.bip138",
        state_crypt::opaque_name("plugin-bundle", id).unwrap()
    ));
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    state_crypt::write_state(&path, &old.to_string()).unwrap();

    let metadata = plugin_bundle::plugin_metadata();
    assert_eq!(metadata.len(), 1);
    assert_eq!(metadata[0].id, "legacy");
    for (key, kind) in [
        ("name", PluginMetadataKind::Text),
        ("abi", PluginMetadataKind::Number),
        ("enabled", PluginMetadataKind::Flag),
        ("modules", PluginMetadataKind::Strings),
    ] {
        let field = metadata[0]
            .fields
            .iter()
            .find(|field| field.key == key)
            .expect("typed metadata field");
        assert_eq!(field.kind, kind, "metadata type for {key}");
    }
    let opened = plugin_bundle::open(id).unwrap();
    assert_eq!(opened.library, b"\x7fELF legacy");
    assert_eq!(opened.library_name, "liblegacy.so");
    assert_eq!(
        opened.catalogs,
        vec![(
            "silent_en.lang".to_string(),
            "hello => \"Hello\"\n".to_string()
        )]
    );

    // Refused bundles, the legacy id's own included, write nothing.
    let before = sealed_files(&state);
    let mut manifest = manifest(&host());
    manifest["id"] = json!(id);
    refused_bytes(
        dir.path(),
        &zip(&items(&manifest, FAKE_LIBRARY)),
        "plugin admission",
    );
    let mut extra = items(&manifest, FAKE_LIBRARY);
    extra.push(Item::new("README", b"x"));
    refused_bytes(dir.path(), &zip(&extra), "undeclared bundle entry 'README'");
    assert_eq!(sealed_files(&state), before);
    assert_eq!(plugin_bundle::open(id).unwrap().library, b"\x7fELF legacy");

    plugin_bundle::remove(id).unwrap();
    assert!(plugin_bundle::list().is_empty());
}

#[test]
#[ignore = "requires SILENT_ABI_FIXTURE_PATH pointing to the compiled ABI 12 fixture"]
fn an_admitted_bundle_is_sealed_replaced_and_never_run() {
    let library = fs::read(
        std::env::var("SILENT_ABI_FIXTURE_PATH").expect("compiled fixture path is required"),
    )
    .expect("read compiled native fixture");
    let state = store();
    let dir = temp_dir::TempDir::new().expect("bundle dir");
    let dir = dir.path();

    // The fixture records its initializer and constructor here, if ever run.
    let sentinel = dir.join("sentinel");
    let previous = std::env::var_os("SILENT_ABI_SENTINEL");
    std::env::set_var("SILENT_ABI_SENTINEL", &sentinel);

    let found = plugin_admission::inspect(&library, &host(), None).expect("fixture admission");
    let bundle = zip(&items(&manifest(&found), &library));

    // An extensionless file: content, not name, makes the bundle.
    let first = write(dir, "example", &bundle);
    assert_eq!(plugin_bundle::install(&first, &host()).unwrap(), "example");
    let listed = plugin_bundle::list();
    assert_eq!(listed.len(), 1);
    let sidecar: Value = serde_json::from_str(&listed[0]).unwrap();
    assert_eq!(sidecar, manifest(&found));
    let metadata = plugin_bundle::plugin_metadata();
    assert_eq!(metadata.len(), 1);
    assert_eq!(metadata[0].id, "example");
    let text = |key: &str| {
        let field = metadata[0]
            .fields
            .iter()
            .find(|field| field.key == key)
            .expect("manifest metadata field");
        (field.kind, field.text.clone(), field.items.clone())
    };
    assert_eq!(
        text("name"),
        (PluginMetadataKind::Text, "Example Plugin".into(), vec![])
    );
    assert_eq!(
        text("version"),
        (PluginMetadataKind::Text, "0.2.0".into(), vec![])
    );
    assert_eq!(
        text("description"),
        (PluginMetadataKind::Text, "An example plugin".into(), vec![])
    );
    assert_eq!(
        text("author"),
        (PluginMetadataKind::Text, "Silent".into(), vec![])
    );
    assert_eq!(
        text("capabilities"),
        (
            PluginMetadataKind::Strings,
            String::new(),
            vec!["feed".into(), "settings".into()]
        )
    );

    let opened = plugin_bundle::open("example").unwrap();
    assert_eq!(opened.library, library, "the exact admitted bytes");
    assert_eq!(opened.library_name, library_file());
    assert_eq!(
        opened.catalogs,
        vec![
            (
                "silent_en.lang".to_string(),
                "hello => \"Hello\"\n".to_string()
            ),
            (
                "silent_fr.lang".to_string(),
                "hello => \"Bonjour\"\n".to_string()
            ),
        ]
    );

    // Sealed, under an opaque name: no id, no metadata in clear.
    let files = sealed_files(&state);
    assert_eq!(files.len(), 1);
    assert!(files[0].0.ends_with(".bip138") && !files[0].0.contains("example"));
    assert!(state_crypt::is_sealed(&files[0].1));
    assert!(!String::from_utf8_lossy(&files[0].1).contains("Example Plugin"));

    // The same bundle under another name, then a new version: one plugin, replaced.
    let renamed = write(dir, "renamed.zip.bin", &bundle);
    assert_eq!(
        plugin_bundle::install(&renamed, &host()).unwrap(),
        "example"
    );
    assert_eq!(sealed_files(&state).len(), 1);
    let mut next = manifest(&found);
    next["version"] = json!("0.3.0");
    next["translations"] = json!(["i18n/silent_en.lang"]);
    let mut next_items = items(&next, &library);
    next_items.truncate(3);
    let next_path = write(dir, "next", &zip(&next_items));
    assert_eq!(
        plugin_bundle::install(&next_path, &host()).unwrap(),
        "example"
    );
    let replaced = sealed_files(&state);
    assert_eq!(replaced.len(), 1);
    assert_eq!(replaced[0].0, files[0].0);
    let sidecar: Value = serde_json::from_str(&plugin_bundle::list()[0]).unwrap();
    assert_eq!(sidecar["version"], "0.3.0");
    assert_eq!(plugin_bundle::open("example").unwrap().catalogs.len(), 1);

    // A manifest that does not describe the library exactly, or a host it does not
    // fit: refused, the installed version kept.
    for (edit, fragment) in [
        (
            Compatibility {
                qt_minor: found.qt_minor.wrapping_add(1),
                ..found.clone()
            },
            "manifest/library compatibility disagreement",
        ),
        (
            Compatibility {
                arch_requirements: found.arch_requirements ^ 1,
                ..found.clone()
            },
            "manifest/library compatibility disagreement",
        ),
        (
            Compatibility {
                iid: "dev.silent.IPlugin/13.0".into(),
                ..found.clone()
            },
            "manifest/library compatibility disagreement",
        ),
    ] {
        refused_bytes(dir, &zip(&items(&manifest(&edit), &library)), fragment);
    }
    let newer_host = Compatibility {
        iid: "dev.silent.IPlugin/13.0".into(),
        ..host()
    };
    let error = plugin_bundle::install(&first, &newer_host).unwrap_err();
    assert!(
        error.contains("expected dev.silent.IPlugin/13.0")
            && error.contains("found dev.silent.IPlugin/12.0"),
        "{error}"
    );
    assert_eq!(sealed_files(&state), replaced);

    // The library itself, dropped loose: refused.
    refused(&write(dir, library_file(), &library), "not a ZIP archive");

    // Inspection and installation never ran the library.
    assert!(!sentinel.exists(), "install must not run plugin code");
    match previous {
        Some(value) => std::env::set_var("SILENT_ABI_SENTINEL", value),
        None => std::env::remove_var("SILENT_ABI_SENTINEL"),
    }

    plugin_bundle::remove("example").unwrap();
    assert!(plugin_bundle::list().is_empty());
}

#[cfg(unix)]
#[test]
fn a_dropped_fifo_is_refused_without_waiting_for_a_writer() {
    let dir = temp_dir::TempDir::new().expect("bundle dir");
    let fifo = dir.path().join("bundle");
    let made = std::process::Command::new("mkfifo")
        .arg(&fifo)
        .status()
        .expect("run mkfifo");
    assert!(made.success(), "mkfifo failed");
    let (sender, receiver) = mpsc::channel();
    let path = fifo.clone();
    std::thread::spawn(move || {
        sender
            .send(plugin_bundle::install(&path, &host()))
            .expect("report install");
    });
    let error = receiver
        .recv_timeout(Duration::from_secs(10))
        .expect("install must not block on a FIFO")
        .expect_err("a FIFO is not a bundle");
    assert!(error.contains("is not a plugin bundle file"), "{error}");
}
