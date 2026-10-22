//! Non-executing native plugin admission. Matching declarations are not a sandbox.

use object::{
    read::macho::{FatArch, MachOFatFile32, MachOFatFile64},
    Architecture, BinaryFormat, FileKind, Object, ObjectKind, ObjectSection, SectionKind,
};

#[path = "plugin_cbor.rs"]
mod cbor;

#[path = "plugin_native.rs"]
mod native;

const MAX_LIBRARY_BYTES: usize = 256 * 1024 * 1024;
const MAX_METADATA_BYTES: usize = 1024 * 1024;
const MAX_SECTIONS: usize = 4096;
const MAX_NATIVE_NAME_BYTES: usize = 1024 * 1024;
const MAGIC: &[u8] = b"QTMETADATA !";
const NOTE_NAME: &[u8] = b"qt-project!\0";
const NOTE_TYPE: u32 = 0x7451_0001;

/// ABI/build declarations, separate from release and bundle-format versions.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct Compatibility {
    pub iid: String,
    pub target_os: String,
    pub target_arch: String,
    pub qt_major: u8,
    pub qt_minor: u8,
    pub arch_requirements: u8,
}

/// Validate library bytes before installation or execution. The manifest, when
/// supplied, must describe the library exactly, not just satisfy the host.
pub fn inspect(
    bytes: &[u8],
    host: &Compatibility,
    manifest: Option<&Compatibility>,
) -> Result<Compatibility, String> {
    let result = inspect_native(bytes, host).and_then(|found| {
        if let Some(manifest) = manifest {
            if manifest != &found {
                return Err(format!("manifest/library compatibility disagreement: expected {manifest:?}, found {found:?}"));
            }
        }
        Ok(found)
    });
    result.map_err(|error| format!("plugin admission (expected {}): {error}", host.iid))
}

fn inspect_native(bytes: &[u8], host: &Compatibility) -> Result<Compatibility, String> {
    if bytes.len() > MAX_LIBRARY_BYTES {
        return Err("native library exceeds admission size limit".into());
    }
    match FileKind::parse(bytes).map_err(|e| format!("malformed native library: {e}"))? {
        FileKind::MachOFat32 if host.target_os == "macos" => {
            let fat = MachOFatFile32::parse(bytes).map_err(|e| e.to_string())?;
            inspect_fat(bytes, fat.arches(), host)
        }
        FileKind::MachOFat64 if host.target_os == "macos" => {
            let fat = MachOFatFile64::parse(bytes).map_err(|e| e.to_string())?;
            inspect_fat(bytes, fat.arches(), host)
        }
        _ => inspect_thin(bytes, host),
    }
}

fn architecture(host: &Compatibility) -> Result<Architecture, String> {
    match host.target_arch.as_str() {
        "x86_64" => Ok(Architecture::X86_64),
        "aarch64" => Ok(Architecture::Aarch64),
        "x86" => Ok(Architecture::I386),
        other => Err(format!("unsupported host architecture {other}")),
    }
}

fn inspect_fat<F: FatArch>(
    bytes: &[u8],
    arches: &[F],
    host: &Compatibility,
) -> Result<Compatibility, String> {
    if arches.len() > 64 {
        return Err("Mach-O slice count exceeds admission limit".into());
    }
    let expected = architecture(host)?;
    let mut selected = None;
    let mut ranges = Vec::new();
    for arch in arches {
        let (start, size) = arch.file_range();
        let alignment = 1_u64
            .checked_shl(arch.align())
            .ok_or("Mach-O slice alignment overflow")?;
        if start < (8 + std::mem::size_of_val(arches)) as u64 || start % alignment != 0 {
            return Err("Mach-O slice overlaps fat headers or violates physical alignment".into());
        }
        let end = start
            .checked_add(size)
            .ok_or("Mach-O slice range overflow")?;
        if end > bytes.len() as u64
            || size == 0
            || ranges
                .iter()
                .any(|&(other_start, other_end)| start < other_end && other_start < end)
        {
            return Err("invalid or overlapping Mach-O slice ranges".into());
        }
        ranges.push((start, end));
        if arch.architecture() == expected {
            if selected.is_some() {
                return Err("ambiguous Mach-O host slices".into());
            }
            selected = Some(arch.data(bytes).map_err(|e| e.to_string())?);
        }
    }
    inspect_thin(selected.ok_or("missing Mach-O host slice")?, host)
}

fn inspect_thin(bytes: &[u8], host: &Compatibility) -> Result<Compatibility, String> {
    native::preflight(bytes)?;
    let file = object::File::parse(bytes).map_err(|e| format!("malformed native library: {e}"))?;
    let format = match host.target_os.as_str() {
        "linux" => BinaryFormat::Elf,
        "windows" => BinaryFormat::Pe,
        "macos" => BinaryFormat::MachO,
        other => return Err(format!("unsupported host OS {other}")),
    };
    let architecture = architecture(host)?;
    if file.format() != format || file.architecture() != architecture || !file.is_little_endian() {
        return Err(format!("native target mismatch: expected {format:?}/{architecture:?}/little-endian, found {:?}/{:?}/{}", file.format(), file.architecture(), if file.is_little_endian() { "little-endian" } else { "big-endian" }));
    }
    if file.kind() != ObjectKind::Dynamic {
        return Err(format!(
            "expected native shared library, found {:?}",
            file.kind()
        ));
    }
    let image = native::NativeImage::parse(&file, bytes)?;
    let mut found = None;
    let mut saw_note_section = false;
    let mut name_bytes = 0;
    for (index, section) in file.sections().enumerate() {
        if index >= MAX_SECTIONS {
            return Err("native section count exceeds admission limit".into());
        }
        let name = section
            .name_bytes()
            .map_err(|e| format!("invalid native section name: {e}"))?;
        name_bytes += name.len();
        if name_bytes > MAX_NATIVE_NAME_BYTES {
            return Err("native section names exceed admission byte limit".into());
        }
        let is_note = format == BinaryFormat::Elf && name == b".note.qt.metadata";
        let is_metadata = match format {
            BinaryFormat::Elf => name == b".qtmetadata",
            BinaryFormat::Pe => name == b".qtmetadata" || name == b".qtmetad",
            BinaryFormat::MachO => {
                name == b"qtmetadata"
                    && section.segment_name().map_err(|e| e.to_string())? == Some("__TEXT")
            }
            _ => false,
        };
        if !is_note && !is_metadata {
            continue;
        }
        image.metadata(&section, is_note)?;
        if section.size() > MAX_METADATA_BYTES as u64
            || section
                .compressed_file_range()
                .map_err(|e| e.to_string())?
                .format
                != object::CompressionFormat::None
        {
            return Err("oversized or compressed Qt metadata section".into());
        }
        let (offset, size) = section
            .file_range()
            .ok_or("Qt metadata has no file range")?;
        if size > MAX_METADATA_BYTES as u64 {
            return Err("Qt metadata file range exceeds admission limit".into());
        }
        if offset
            .checked_add(size)
            .is_none_or(|end| end > bytes.len() as u64)
        {
            return Err("Qt metadata section outside native library".into());
        }
        let data = section
            .data()
            .map_err(|e| format!("invalid Qt metadata section: {e}"))?;
        if is_note {
            saw_note_section = true;
            if section.kind() != SectionKind::Note {
                return Err("Qt metadata section is not an ELF note".into());
            }
            read_notes(data, section.align(), host, &mut found)?;
        } else {
            read_records(data, host, &mut found)?;
        }
    }
    let found = found.ok_or_else(|| "found missing Qt plugin metadata".to_string())?;
    image.metadata_notes(saw_note_section)?;
    image.exports(&file, bytes)?;
    Ok(found)
}

fn read_notes(
    data: &[u8],
    alignment: u64,
    host: &Compatibility,
    found: &mut Option<Compatibility>,
) -> Result<(), String> {
    if alignment != 4 && alignment != 8 {
        return Err("invalid Qt ELF note alignment".into());
    }
    let alignment = alignment as usize;
    let mut offset = 0;
    while offset < data.len() {
        if data[offset..].iter().all(|byte| *byte == 0) {
            break;
        }
        let header = data
            .get(offset..offset + 12)
            .ok_or("truncated ELF note header")?;
        let word = |start| {
            u32::from_le_bytes([
                header[start],
                header[start + 1],
                header[start + 2],
                header[start + 3],
            ]) as usize
        };
        let name_size = word(0);
        let desc_size = word(4);
        let note_type = word(8) as u32;
        let name_start = offset + 12;
        let name_end = name_start
            .checked_add(name_size)
            .ok_or("ELF note name overflow")?;
        let desc_start = name_end
            .checked_add(alignment - 1)
            .ok_or("ELF note alignment overflow")?
            & !(alignment - 1);
        let desc_end = desc_start
            .checked_add(desc_size)
            .ok_or("ELF note descriptor overflow")?;
        let name = data
            .get(name_start..name_end)
            .ok_or("ELF note name outside section")?;
        let descriptor = data
            .get(desc_start..desc_end)
            .ok_or("ELF note descriptor outside section")?;
        if name != NOTE_NAME || note_type != NOTE_TYPE {
            return Err("invalid Qt ELF note identity".into());
        }
        let record = read_record(descriptor, host)?;
        if record.consumed != descriptor.len() {
            return Err("invalid Qt ELF note descriptor size".into());
        }
        add_record(found, record.compatibility)?;
        offset = desc_end
            .checked_add(alignment - 1)
            .ok_or("ELF note alignment overflow")?
            & !(alignment - 1);
        if offset > data.len() && desc_end != data.len() {
            return Err("truncated ELF note padding".into());
        }
    }
    Ok(())
}

fn read_records(
    data: &[u8],
    host: &Compatibility,
    found: &mut Option<Compatibility>,
) -> Result<(), String> {
    let mut offset = 0;
    while offset < data.len() {
        if data[offset..].iter().all(|byte| *byte == 0) {
            break;
        }
        if !data[offset..].starts_with(MAGIC) {
            return Err("malformed Qt metadata section marker".into());
        }
        offset += MAGIC.len();
        let record = read_record(&data[offset..], host)?;
        add_record(found, record.compatibility)?;
        offset += record.consumed;
        // Native linkers may align consecutive metadata records.
        while offset < data.len() && data[offset] == 0 {
            offset += 1;
        }
    }
    Ok(())
}

fn add_record(found: &mut Option<Compatibility>, metadata: Compatibility) -> Result<(), String> {
    if found.is_some() {
        return Err("ambiguous Qt plugin metadata records".into());
    }
    *found = Some(metadata);
    Ok(())
}

struct MetadataRecord {
    compatibility: Compatibility,
    consumed: usize,
}

fn read_record(data: &[u8], host: &Compatibility) -> Result<MetadataRecord, String> {
    let header = data.get(..4).ok_or("truncated Qt plugin metadata header")?;
    if header[0] != 1 {
        return Err(format!(
            "Qt metadata format mismatch: expected 1, found {}",
            header[0]
        ));
    }
    if header[1] != host.qt_major || header[2] > host.qt_minor {
        return Err(format!(
            "Qt version mismatch: expected {}.{} or older minor, found {}.{}",
            host.qt_major, host.qt_minor, header[1], header[2]
        ));
    }
    let requirements = header[3];
    let cpu_level = requirements & 0x7f;
    if cpu_level > 4 || cpu_level > host.arch_requirements & 0x7f {
        return Err(format!(
            "Qt CPU requirements mismatch: supported {}, found {cpu_level}",
            host.arch_requirements & 0x7f
        ));
    }
    if host.target_os == "windows" && requirements & 0x80 != host.arch_requirements & 0x80 {
        return Err(format!(
            "Qt debug/release mismatch: expected {}, found {}",
            host.arch_requirements & 0x80,
            requirements & 0x80
        ));
    }
    let root = cbor::read(&data[4..]).map_err(|e| format!("malformed Qt metadata CBOR: {e}"))?;
    if root.iid != host.iid {
        return Err(format!(
            "incompatible Qt plugin root IID: expected {}, found {}",
            host.iid, root.iid
        ));
    }
    Ok(MetadataRecord {
        compatibility: Compatibility {
            iid: root.iid,
            target_os: host.target_os.clone(),
            target_arch: host.target_arch.clone(),
            qt_major: header[1],
            qt_minor: header[2],
            arch_requirements: requirements,
        },
        consumed: 4 + root.consumed,
    })
}

pub fn plugin_admission(
    bytes: &[u8],
    expected_iid: String,
    qt_major: u8,
    qt_minor: u8,
    arch_requirements: u8,
) -> String {
    let host = Compatibility {
        iid: expected_iid,
        target_os: std::env::consts::OS.into(),
        target_arch: std::env::consts::ARCH.into(),
        qt_major,
        qt_minor,
        arch_requirements,
    };
    inspect(bytes, &host, None).err().unwrap_or_default()
}

#[cfg(test)]
#[path = "plugin_admission_tests.rs"]
mod tests;
