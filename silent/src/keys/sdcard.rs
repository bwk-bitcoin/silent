//! SD card PSBT save and load: the airgap route with no QR exchange. The C++ side
//! always supplies a full path chosen through a `QFileDialog`, so nothing here builds
//! a filename or picks a directory.

use std::fs::OpenOptions;
use std::io::Write;
use std::path::Path;

use base64::prelude::BASE64_STANDARD;
use base64::Engine;

use crate::keys::error::Error;
use crate::keys::psbt;

/// A PSBT file larger than this is refused unread: the protocol message cap is the
/// same 512 KiB, so anything bigger is not a PSBT this wallet produced.
const MAX_PSBT_FILE_BYTES: u64 = 512 * 1024;

/// Named in the error when no form parses, so the user sees what was attempted.
const TRIED_FORMS: &str = "PSBT v2, PSBT v0, base64, hex";

/// Writes `bytes` to `path` exactly as given. Refuses to overwrite: an existing file
/// is an error, never a silent replacement of someone else's PSBT.
pub fn save_psbt(path: &Path, bytes: &[u8]) -> Result<(), Error> {
    let mut file = match OpenOptions::new().write(true).create_new(true).open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
            return Err(Error::FileExists {
                path: path.to_path_buf(),
            })
        }
        Err(source) => {
            return Err(Error::Io {
                path: path.to_path_buf(),
                source,
            })
        }
    };
    file.write_all(bytes).map_err(|source| Error::Io {
        path: path.to_path_buf(),
        source,
    })
}

/// Reads a PSBT saved by any of the four accepted forms and returns the serialized
/// bytes as they were stored, never re-serialized through the parser.
pub fn load_psbt(path: &Path) -> Result<Vec<u8>, Error> {
    let metadata = std::fs::metadata(path).map_err(|source| Error::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if metadata.len() > MAX_PSBT_FILE_BYTES {
        return Err(Error::PsbtFileTooLarge {
            path: path.to_path_buf(),
            len: metadata.len(),
        });
    }

    let raw = std::fs::read(path).map_err(|source| Error::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if is_psbt(&raw) {
        return Ok(raw);
    }

    if let Ok(text) = std::str::from_utf8(&raw) {
        let trimmed = text.trim_matches(|c: char| c.is_ascii_whitespace());
        if let Ok(decoded) = BASE64_STANDARD.decode(trimmed) {
            if is_psbt(&decoded) {
                return Ok(decoded);
            }
        }
        if let Ok(decoded) = hex::decode(trimmed) {
            if is_psbt(&decoded) {
                return Ok(decoded);
            }
        }
    }

    Err(Error::UnreadablePsbtFile {
        path: path.to_path_buf(),
        tried: TRIED_FORMS,
    })
}

fn is_psbt(bytes: &[u8]) -> bool {
    psbt::parse_v2(bytes).is_ok() || bitcoin::Psbt::deserialize(bytes).is_ok()
}
