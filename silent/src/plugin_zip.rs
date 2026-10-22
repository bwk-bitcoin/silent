//! Strict in-memory reader for plugin distribution archives (ws-plaude/silent#69).
//!
//! Hand-written over `flate2` rather than a ZIP crate so every archive byte is
//! accounted for: local and central records must agree and tile the archive with no
//! gap, overlap or trailing data, and malformed input errors instead of panicking
//! across the CXX bridge. Nothing is extracted to disk.

use flate2::{Decompress, FlushDecompress, Status};

const LOCAL_SIGNATURE: u32 = 0x0403_4b50;
const CENTRAL_SIGNATURE: u32 = 0x0201_4b50;
const END_SIGNATURE: u32 = 0x0605_4b50;
const DESCRIPTOR_SIGNATURE: u32 = 0x0807_4b50;
const LOCAL_HEADER_BYTES: usize = 30;
const CENTRAL_HEADER_BYTES: usize = 46;
const END_RECORD_BYTES: usize = 22;

const FLAG_DEFLATE_OPTIONS: u16 = 0b110;
const FLAG_DESCRIPTOR: u16 = 1 << 3;
const FLAG_UTF8: u16 = 1 << 11;
const ALLOWED_FLAGS: u16 = FLAG_DEFLATE_OPTIONS | FLAG_DESCRIPTOR | FLAG_UTF8;

const METHOD_STORED: u16 = 0;
const METHOD_DEFLATE: u16 = 8;

const EXTRA_ZIP64: u16 = 0x0001;
const EXTRA_STRONG_ENCRYPTION: u16 = 0x0017;

const UNIX_TYPE_MASK: u32 = 0o170_000;
const UNIX_REGULAR: u32 = 0o100_000;
const DOS_DIRECTORY: u32 = 0x10;

const INFLATE_CHUNK: usize = 64 * 1024;

/// A parsed archive: entries are validated but not decoded yet.
#[derive(Debug)]
pub struct Archive<'a> {
    pub entries: Vec<Entry<'a>>,
}

/// One regular file of the archive.
#[derive(Debug)]
pub struct Entry<'a> {
    pub name: &'a str,
    /// The declared decoded size, checked against the actual one by `decode`.
    pub size: u64,
    method: u16,
    crc32: u32,
    data: &'a [u8],
}

/// The central header fields the local header and the layout are checked against.
struct Central<'a> {
    name: &'a str,
    flags: u16,
    method: u16,
    crc32: u32,
    compressed: u64,
    size: u64,
    local: usize,
}

fn slice(bytes: &[u8], at: usize, len: usize) -> Result<&[u8], String> {
    at.checked_add(len)
        .and_then(|end| bytes.get(at..end))
        .ok_or_else(|| "truncated archive record".to_string())
}

fn le16(bytes: &[u8], at: usize) -> Result<u16, String> {
    let field = slice(bytes, at, 2)?;
    Ok(u16::from_le_bytes([field[0], field[1]]))
}

fn le32(bytes: &[u8], at: usize) -> Result<u32, String> {
    let field = slice(bytes, at, 4)?;
    Ok(u32::from_le_bytes([field[0], field[1], field[2], field[3]]))
}

/// Refuses ZIP64 and strongly encrypted entries, whatever their header flags say.
fn check_extra(extra: &[u8]) -> Result<(), String> {
    let mut at = 0;
    while at < extra.len() {
        let id = le16(extra, at)?;
        let len = le16(extra, at + 2)? as usize;
        slice(extra, at + 4, len)?;
        if id == EXTRA_ZIP64 || id == EXTRA_STRONG_ENCRYPTION {
            return Err("ZIP64 and encrypted archive entries are not supported".into());
        }
        at += 4 + len;
    }
    Ok(())
}

/// Refuses directory records and absolute, traversing, empty or control-character
/// entry names.
fn check_name(name: &str) -> Result<(), String> {
    if name.ends_with('/') {
        return Err(format!("archive directory entry '{name}'"));
    }
    let unsafe_part = name
        .split('/')
        .any(|part| part.is_empty() || part == "." || part == "..");
    if unsafe_part || name.contains('\\') || name.chars().any(char::is_control) {
        return Err(format!("unsafe archive entry name '{name}'"));
    }
    Ok(())
}

/// Refuses links, directories and special files, by their DOS or Unix attributes.
/// Unix type bits count whatever host the entry claims to come from.
fn check_file_type(name: &str, attributes: u32) -> Result<(), String> {
    let unix_type = (attributes >> 16) & UNIX_TYPE_MASK;
    let unix_special = unix_type != 0 && unix_type != UNIX_REGULAR;
    if unix_special || attributes & DOS_DIRECTORY != 0 {
        return Err(format!("archive entry '{name}' is not a regular file"));
    }
    Ok(())
}

fn check_method(name: &str, method: u16, compressed: u64, size: u64) -> Result<(), String> {
    match method {
        METHOD_STORED if compressed != size => Err(format!(
            "stored archive entry '{name}' has inconsistent sizes"
        )),
        METHOD_STORED | METHOD_DEFLATE => Ok(()),
        other => Err(format!(
            "archive entry '{name}' uses unsupported compression method {other}"
        )),
    }
}

/// Reads the central header at `at`: answers it and where the next one starts.
fn read_central(bytes: &[u8], at: usize) -> Result<(Central<'_>, usize), String> {
    if le32(bytes, at)? != CENTRAL_SIGNATURE {
        return Err("malformed archive central directory".into());
    }
    let flags = le16(bytes, at + 8)?;
    let method = le16(bytes, at + 10)?;
    let crc32 = le32(bytes, at + 16)?;
    let compressed = le32(bytes, at + 20)? as u64;
    let size = le32(bytes, at + 24)? as u64;
    let name_len = le16(bytes, at + 28)? as usize;
    let extra_len = le16(bytes, at + 30)? as usize;
    let comment_len = le16(bytes, at + 32)? as usize;
    let disk = le16(bytes, at + 34)?;
    let attributes = le32(bytes, at + 38)?;
    let local = le32(bytes, at + 42)? as usize;
    let name = std::str::from_utf8(slice(bytes, at + CENTRAL_HEADER_BYTES, name_len)?)
        .map_err(|_| "non UTF-8 archive entry name".to_string())?;
    check_extra(slice(
        bytes,
        at + CENTRAL_HEADER_BYTES + name_len,
        extra_len,
    )?)?;
    let next = at + CENTRAL_HEADER_BYTES + name_len + extra_len + comment_len;
    slice(bytes, at, next - at)?;
    check_name(name)?;
    check_file_type(name, attributes)?;
    if disk != 0 {
        return Err("multi-disk archives are not supported".into());
    }
    if flags & !ALLOWED_FLAGS != 0 {
        return Err(format!(
            "archive entry '{name}' is encrypted or uses unsupported flags"
        ));
    }
    check_method(name, method, compressed, size)?;
    let central = Central {
        name,
        flags,
        method,
        crc32,
        compressed,
        size,
        local,
    };
    Ok((central, next))
}

/// Checks the local header and data descriptor of `central` against it: answers the
/// entry data and where the entry ends.
fn read_local<'a>(bytes: &'a [u8], central: &Central) -> Result<(&'a [u8], usize), String> {
    let at = central.local;
    if le32(bytes, at)? != LOCAL_SIGNATURE {
        return Err("malformed archive local header".into());
    }
    let flags = le16(bytes, at + 6)?;
    let method = le16(bytes, at + 8)?;
    let declared = (
        le32(bytes, at + 14)?,
        le32(bytes, at + 18)? as u64,
        le32(bytes, at + 22)? as u64,
    );
    let name_len = le16(bytes, at + 26)? as usize;
    let extra_len = le16(bytes, at + 28)? as usize;
    let name = slice(bytes, at + LOCAL_HEADER_BYTES, name_len)?;
    check_extra(slice(bytes, at + LOCAL_HEADER_BYTES + name_len, extra_len)?)?;
    if flags != central.flags || method != central.method || name != central.name.as_bytes() {
        return Err(format!(
            "archive entry '{}' local and central headers disagree",
            central.name
        ));
    }
    let descriptor = central.flags & FLAG_DESCRIPTOR != 0;
    let expected = (central.crc32, central.compressed, central.size);
    // With a data descriptor, the local header may leave CRC and sizes zero.
    if declared != expected && !(descriptor && declared == (0, 0, 0)) {
        return Err(format!(
            "archive entry '{}' local and central sizes disagree",
            central.name
        ));
    }
    let start = at + LOCAL_HEADER_BYTES + name_len + extra_len;
    let data = slice(bytes, start, central.compressed as usize)?;
    let mut end = start + data.len();
    if descriptor {
        if le32(bytes, end)? == DESCRIPTOR_SIGNATURE {
            end += 4;
        }
        let found = (
            le32(bytes, end)?,
            le32(bytes, end + 4)? as u64,
            le32(bytes, end + 8)? as u64,
        );
        if found != expected {
            return Err(format!(
                "archive entry '{}' data descriptor disagrees with its header",
                central.name
            ));
        }
        end += 12;
    }
    Ok((data, end))
}

impl<'a> Archive<'a> {
    /// Parses `bytes` without decoding any entry: at most `max_entries` regular files
    /// with unique safe names, and no archive byte outside a record.
    pub fn parse(bytes: &'a [u8], max_entries: usize) -> Result<Self, String> {
        let end = bytes
            .len()
            .checked_sub(END_RECORD_BYTES)
            .ok_or("not a ZIP archive")?;
        // No archive comment: the end record is the last bytes, never searched for.
        if le32(bytes, end)? != END_SIGNATURE || le16(bytes, end + 20)? != 0 {
            return Err("not a ZIP archive without a trailing comment".into());
        }
        let disk = le16(bytes, end + 4)?;
        let directory_disk = le16(bytes, end + 6)?;
        let disk_count = le16(bytes, end + 8)? as usize;
        let count = le16(bytes, end + 10)? as usize;
        let directory_size = le32(bytes, end + 12)? as usize;
        let directory = le32(bytes, end + 16)? as usize;
        if disk != 0 || directory_disk != 0 || disk_count != count {
            return Err("multi-disk archives are not supported".into());
        }
        if count == 0 || count > max_entries {
            return Err(format!(
                "archive holds {count} entries, expected 1 to {max_entries}"
            ));
        }
        if directory.checked_add(directory_size) != Some(end) {
            return Err("archive central directory does not end at its end record".into());
        }
        let centrals = Self::read_directory(bytes, directory, count, end)?;
        let mut covered = 0;
        let mut entries = Vec::with_capacity(count);
        for central in centrals {
            if central.local != covered {
                return Err("archive entries leave gaps or overlap".into());
            }
            let (data, entry_end) = read_local(bytes, &central)?;
            covered = entry_end;
            entries.push(Entry {
                name: central.name,
                size: central.size,
                method: central.method,
                crc32: central.crc32,
                data,
            });
        }
        if covered != directory {
            return Err("archive entries leave gaps or overlap".into());
        }
        Ok(Archive { entries })
    }

    /// The `count` central headers from `directory` to `end`, by local offset. Names
    /// equal ignoring ASCII case are ambiguous on some file systems: refused.
    fn read_directory(
        bytes: &'a [u8],
        directory: usize,
        count: usize,
        end: usize,
    ) -> Result<Vec<Central<'a>>, String> {
        let mut centrals: Vec<Central> = Vec::with_capacity(count);
        let mut at = directory;
        for _ in 0..count {
            let (central, next) = read_central(bytes, at)?;
            if centrals
                .iter()
                .any(|other| other.name.eq_ignore_ascii_case(central.name))
            {
                return Err(format!("duplicate archive entry '{}'", central.name));
            }
            centrals.push(central);
            at = next;
        }
        if at != end {
            return Err("archive central directory size disagrees with its entries".into());
        }
        centrals.sort_by_key(|central| central.local);
        Ok(centrals)
    }

    pub fn entry(&self, name: &str) -> Option<&Entry<'a>> {
        self.entries.iter().find(|entry| entry.name == name)
    }

    /// Decodes the entry `name`, as `Entry::decode`.
    pub fn decode(&self, name: &str, limit: usize) -> Result<Vec<u8>, String> {
        self.entry(name)
            .ok_or_else(|| format!("missing archive entry '{name}'"))?
            .decode(limit)
    }
}

impl Entry<'_> {
    /// Decodes the entry, producing at most its declared size plus one byte whatever
    /// the stream holds. The output must match the declared size and CRC.
    pub fn decode(&self, limit: usize) -> Result<Vec<u8>, String> {
        let name = self.name;
        if self.size > limit as u64 {
            return Err(format!(
                "archive entry '{name}' exceeds its {limit} byte limit"
            ));
        }
        let data = match self.method {
            METHOD_STORED => self.data.to_vec(),
            _ => self.inflate()?,
        };
        if data.len() as u64 != self.size {
            return Err(format!(
                "archive entry '{name}' size disagrees with its header"
            ));
        }
        if crc32fast::hash(&data) != self.crc32 {
            return Err(format!("archive entry '{name}' fails its CRC check"));
        }
        Ok(data)
    }

    /// Inflates the whole compressed data into at most the declared size plus one byte:
    /// the stream must end exactly where the entry data ends. Memory follows the
    /// actual output, chunk by chunk, never the declared size alone.
    fn inflate(&self) -> Result<Vec<u8>, String> {
        let name = self.name;
        let cap = self.size as usize + 1;
        let mut inflater = Decompress::new(false);
        let mut chunk = vec![0; INFLATE_CHUNK];
        let mut data = Vec::new();
        loop {
            let consumed = inflater.total_in() as usize;
            let before = inflater.total_out();
            let room = INFLATE_CHUNK.min(cap - data.len());
            let status = inflater
                .decompress(
                    &self.data[consumed..],
                    &mut chunk[..room],
                    FlushDecompress::None,
                )
                .map_err(|e| format!("archive entry '{name}': {e}"))?;
            let produced = (inflater.total_out() - before) as usize;
            data.extend_from_slice(&chunk[..produced]);
            if status == Status::StreamEnd {
                break;
            }
            let stalled = inflater.total_in() as usize == consumed && produced == 0;
            if stalled || data.len() == cap {
                return Err(format!(
                    "archive entry '{name}' is truncated or larger than its header"
                ));
            }
        }
        if inflater.total_in() != self.data.len() as u64 {
            return Err(format!(
                "archive entry '{name}' has data after its compressed stream"
            ));
        }
        Ok(data)
    }
}
