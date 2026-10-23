//! The Windows disk access of the plugin storage, relative to directory handles.
//!
//! The configured data directory is the host's own choice: it is opened once by path,
//! links followed. Everything below it, the plugin root, the legacy state directory,
//! the document and its temp, is opened with `NtCreateFile` relative to the handle of
//! its directory, by a single name, on the reparse point itself and never through it.
//! A handle that turns out to be a reparse point is refused. No path is resolved again
//! after the data directory, so renaming a directory or planting a junction or a link
//! while an operation runs never redirects it.
//!
//! This is separate from `state_crypt`'s writer: `MoveFileExW` takes paths, so it would
//! follow a junction planted in a parent between the checks and the move. Std offers no
//! handle-relative open or rename on Windows, so this module calls ntdll directly.

use std::{
    ffi::c_void,
    fs::{File, OpenOptions},
    io::{self, Read, Write},
    mem::size_of,
    os::windows::{
        fs::{MetadataExt, OpenOptionsExt},
        io::{AsRawHandle, FromRawHandle, OwnedHandle, RawHandle},
    },
    path::{Path, PathBuf},
    ptr::{null, null_mut},
};

use crate::{
    state_crypt,
    storage::{
        lock_file, ErrorCode, DOCUMENT, DOCUMENT_LOCK, DOCUMENT_TEMP, MAX_ENVELOPE_BYTES,
        MAX_TREE_BYTES,
    },
};

const DELETE: u32 = 0x0001_0000;
const SYNCHRONIZE: u32 = 0x0010_0000;
const FILE_READ_DATA: u32 = 0x1;
const FILE_WRITE_DATA: u32 = 0x2;
const FILE_LIST_DIRECTORY: u32 = 0x1;
const FILE_ADD_FILE: u32 = 0x2;
const FILE_ADD_SUBDIRECTORY: u32 = 0x4;
const FILE_TRAVERSE: u32 = 0x20;
const FILE_READ_ATTRIBUTES: u32 = 0x80;

const FILE_SHARE_READ: u32 = 0x1;
const FILE_SHARE_WRITE: u32 = 0x2;
const FILE_SHARE_DELETE: u32 = 0x4;
const FILE_SHARE_ALL: u32 = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

const FILE_OPEN: u32 = 1;
const FILE_CREATE: u32 = 2;
const FILE_OPEN_IF: u32 = 3;

const FILE_DIRECTORY_FILE: u32 = 0x1;
const FILE_WRITE_THROUGH: u32 = 0x2;
const FILE_SYNCHRONOUS_IO_NONALERT: u32 = 0x20;
const FILE_NON_DIRECTORY_FILE: u32 = 0x40;
const FILE_OPEN_REPARSE_POINT: u32 = 0x0020_0000;

const OBJ_CASE_INSENSITIVE: u32 = 0x40;
const FILE_FLAG_BACKUP_SEMANTICS: u32 = 0x0200_0000;
const FILE_ATTRIBUTE_DIRECTORY: u32 = 0x10;
const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;

const FILE_RENAME_INFORMATION: u32 = 10;
const FILE_DISPOSITION_INFORMATION: u32 = 13;

const STATUS_OBJECT_NAME_INVALID: i32 = 0xC000_0033_u32 as i32;
const STATUS_OBJECT_NAME_NOT_FOUND: i32 = 0xC000_0034_u32 as i32;
const STATUS_FILE_IS_A_DIRECTORY: i32 = 0xC000_00BA_u32 as i32;
const STATUS_NOT_A_DIRECTORY: i32 = 0xC000_0103_u32 as i32;

/// Listing, traversing and reading attributes, synchronous.
const DIRECTORY_READ: u32 =
    FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
/// Also adding entries: flushing a directory needs a handle with write access.
const DIRECTORY_WRITE: u32 = DIRECTORY_READ | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
/// The longest name a rename here takes, in UTF-16 units.
const RENAME_NAME_CAPACITY: usize = 64;

/// `UNICODE_STRING`: `length` and `maximum_length` count bytes, not units, and the
/// buffer needs no terminating NUL. Same layout as C on 32 and 64 bit: two `u16` then
/// a pointer at its natural alignment.
#[repr(C)]
struct UnicodeString {
    length: u16,
    maximum_length: u16,
    buffer: *mut u16,
}

/// `OBJECT_ATTRIBUTES`, `length` its own size. Pointers only borrow: everything they
/// point to must outlive the call that reads the structure.
#[repr(C)]
struct ObjectAttributes {
    length: u32,
    root_directory: RawHandle,
    object_name: *const UnicodeString,
    attributes: u32,
    security_descriptor: *const c_void,
    security_quality_of_service: *const c_void,
}

/// `IO_STATUS_BLOCK`: a union of an `NTSTATUS` and a pointer, then a `ULONG_PTR`. Both
/// fields are pointer sized; the calls here are synchronous, so their returned status
/// is final and this block is only written by the system, never read.
#[repr(C)]
struct IoStatusBlock {
    status: usize,
    information: usize,
}

/// `FILE_RENAME_INFORMATION` with room for a name of `RENAME_NAME_CAPACITY` units.
/// C has a union of a `BOOLEAN ReplaceIfExists` and a `ULONG Flags` at offset 0, the
/// `ULONG` setting the union's size; `1` reads as TRUE either way on little endian.
/// `file_name_length` counts bytes and the name needs no NUL. The size passed with it
/// covers the whole array, never less than the name's offset plus its length.
#[repr(C)]
struct RenameInformation {
    replace_if_exists: u32,
    root_directory: RawHandle,
    file_name_length: u32,
    file_name: [u16; RENAME_NAME_CAPACITY],
}

/// `FILE_DISPOSITION_INFORMATION`: one `BOOLEAN`.
#[repr(C)]
struct DispositionInformation {
    delete_file: u8,
}

#[link(name = "ntdll")]
extern "system" {
    fn NtCreateFile(
        file_handle: *mut RawHandle,
        desired_access: u32,
        object_attributes: *const ObjectAttributes,
        io_status_block: *mut IoStatusBlock,
        allocation_size: *const i64,
        file_attributes: u32,
        share_access: u32,
        create_disposition: u32,
        create_options: u32,
        ea_buffer: *const c_void,
        ea_length: u32,
    ) -> i32;

    fn NtSetInformationFile(
        file_handle: RawHandle,
        io_status_block: *mut IoStatusBlock,
        file_information: *const c_void,
        length: u32,
        file_information_class: u32,
    ) -> i32;
}

pub(crate) struct LockedRoot {
    root: Directory,
    _lock: File,
}

pub(crate) fn lock_app_state(data_dir: &Path) -> Result<File, ErrorCode> {
    let data = Directory::data(data_dir, DIRECTORY_WRITE)?.ok_or(ErrorCode::Io)?;
    let path = data.path.join("app.lock");
    let file = data
        .open(
            "app.lock",
            FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_OPEN_IF,
            FILE_NON_DIRECTORY_FILE,
        )
        .map_err(|status| status.refused(&path))?;
    refuse_reparse_point(&file, &path)?;
    fs2::FileExt::lock_exclusive(&file)?;
    Ok(file)
}

impl LockedRoot {
    pub(crate) fn lease(&self, name: &str) -> Result<File, ErrorCode> {
        let path = self.root.path.join(name);
        let file = self
            .root
            .open(
                name,
                FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                FILE_OPEN_IF,
                FILE_NON_DIRECTORY_FILE,
            )
            .map_err(|status| status.refused(&path))?;
        refuse_reparse_point(&file, &path)?;
        lock_file(&file)?;
        Ok(file)
    }

    pub(crate) fn open(
        data_dir: &Path,
        plugin_id: &str,
        create: bool,
    ) -> Result<Option<Self>, ErrorCode> {
        let access = if create {
            DIRECTORY_WRITE
        } else {
            DIRECTORY_READ
        };
        let Some(data) = Directory::data(data_dir, access)? else {
            return if create { Err(ErrorCode::Io) } else { Ok(None) };
        };
        let disposition = if create { FILE_OPEN_IF } else { FILE_OPEN };
        // Creating the persistent lock needs write access even for a read request.
        let Some(root) = data.directory(plugin_id, DIRECTORY_WRITE, disposition)? else {
            return Ok(None);
        };
        if create {
            data.sync()?;
        }
        let path = root.path.join(DOCUMENT_LOCK);
        let lock = root
            .open(
                DOCUMENT_LOCK,
                FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                FILE_OPEN_IF,
                FILE_NON_DIRECTORY_FILE,
            )
            .map_err(|status| status.refused(&path))?;
        refuse_reparse_point(&lock, &path)?;
        lock_file(&lock)?;
        Ok(Some(Self { root, _lock: lock }))
    }

    pub(crate) fn read(&self) -> Result<Option<Vec<u8>>, ErrorCode> {
        let limit = (MAX_TREE_BYTES + MAX_ENVELOPE_BYTES) as u64;
        let sealed = self.root.read_file(DOCUMENT, limit)?;
        if sealed
            .as_ref()
            .is_some_and(|sealed| sealed.len() as u64 > limit)
        {
            return Err(ErrorCode::Corrupt);
        }
        Ok(sealed)
    }

    pub(crate) fn read_temporary(&self) -> Result<Option<Vec<u8>>, ErrorCode> {
        let limit = (MAX_TREE_BYTES + MAX_ENVELOPE_BYTES) as u64;
        let bytes = self.root.read_file(DOCUMENT_TEMP, limit)?;
        if bytes
            .as_ref()
            .is_some_and(|bytes| bytes.len() as u64 > limit)
        {
            return Err(ErrorCode::TooLarge);
        }
        Ok(bytes)
    }

    pub(crate) fn remove_temporary(&self) -> Result<(), ErrorCode> {
        self.root.remove(DOCUMENT_TEMP)?;
        self.root.sync()
    }

    /// The lock and root handles remain held from load through durable publication.
    pub(crate) fn replace<G>(
        &self,
        sealed: &[u8],
        authorize: impl FnOnce() -> Result<G, ErrorCode>,
    ) -> Result<(), ErrorCode> {
        self.root.remove(DOCUMENT_TEMP)?;
        let temp = self.root.create_file(DOCUMENT_TEMP, sealed)?;
        let guard = authorize()?;
        self.root.rename_into(&temp, DOCUMENT)?;
        drop(guard);
        sync(&temp, &self.root.path.join(DOCUMENT))?;
        self.root.sync()
    }
}

pub(crate) fn remove_legacy(data_dir: &Path, name: &str) -> Result<(), ErrorCode> {
    let Some(data) = Directory::data(data_dir, DIRECTORY_READ)? else {
        return Ok(());
    };
    let Some(legacy) = data.directory(state_crypt::PLUGIN_STATE_DIR, DIRECTORY_WRITE, FILE_OPEN)?
    else {
        return Ok(());
    };
    legacy.remove(name)?;
    legacy.sync()
}

/// The bytes of `filename` in the legacy plugin state directory, never through a link:
/// `None` when there is none, `too_large` past `limit` bytes.
pub(crate) fn read_legacy(
    data_dir: &Path,
    filename: &str,
    limit: u64,
) -> Result<Option<Vec<u8>>, ErrorCode> {
    let Some(data) = Directory::data(data_dir, DIRECTORY_READ)? else {
        return Ok(None);
    };
    let Some(legacy) = data.directory(state_crypt::PLUGIN_STATE_DIR, DIRECTORY_READ, FILE_OPEN)?
    else {
        return Ok(None);
    };
    let bytes = legacy.read_file(filename, limit)?;
    if bytes
        .as_ref()
        .is_some_and(|bytes| bytes.len() as u64 > limit)
    {
        return Err(ErrorCode::TooLarge);
    }
    Ok(bytes)
}

/// A directory open by handle. `path` only names it in logs: it is never opened.
struct Directory {
    handle: File,
    path: PathBuf,
}

impl Directory {
    /// The data directory, opened once by its path, `None` when absent.
    fn data(data_dir: &Path, access: u32) -> Result<Option<Self>, ErrorCode> {
        let handle = match OpenOptions::new()
            .access_mode(access)
            .custom_flags(FILE_FLAG_BACKUP_SEMANTICS)
            .open(data_dir)
        {
            Ok(handle) => handle,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(error) => return Err(failed(data_dir, &error)),
        };
        if attributes(&handle, data_dir)? & FILE_ATTRIBUTE_DIRECTORY == 0 {
            log::error!("storage: {} is not a directory", data_dir.display());
            return Err(ErrorCode::Unavailable);
        }
        Ok(Some(Self {
            handle,
            path: data_dir.to_path_buf(),
        }))
    }

    /// The directory `name` in this one, created with `FILE_OPEN_IF`: `None` when
    /// absent, `unavailable` when a reparse point or another kind of file is there.
    fn directory(
        &self,
        name: &str,
        access: u32,
        disposition: u32,
    ) -> Result<Option<Self>, ErrorCode> {
        let path = self.path.join(name);
        let handle = match self.open(
            name,
            access,
            FILE_SHARE_ALL,
            disposition,
            FILE_DIRECTORY_FILE,
        ) {
            Ok(handle) => handle,
            Err(status) if status.is_not_found() => return Ok(None),
            Err(status) => return Err(status.refused(&path)),
        };
        refuse_reparse_point(&handle, &path)?;
        Ok(Some(Self { handle, path }))
    }

    /// The bytes of the regular file `name`, reading at most one byte past `limit`:
    /// `None` when absent.
    fn read_file(&self, name: &str, limit: u64) -> Result<Option<Vec<u8>>, ErrorCode> {
        let path = self.path.join(name);
        let access = FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        let file = match self.open(
            name,
            access,
            FILE_SHARE_READ,
            FILE_OPEN,
            FILE_NON_DIRECTORY_FILE,
        ) {
            Ok(file) => file,
            Err(status) if status.is_not_found() => return Ok(None),
            Err(status) => return Err(status.refused(&path)),
        };
        refuse_reparse_point(&file, &path)?;
        let mut bytes = Vec::new();
        file.take(limit.saturating_add(1))
            .read_to_end(&mut bytes)
            .map_err(|error| failed(&path, &error))?;
        Ok(Some(bytes))
    }

    /// Deletes the entry `name` itself, whatever it is, never what a link there points
    /// to. The name goes once no handle on it is left, at the latest when this returns
    /// unless another process holds it open.
    fn remove(&self, name: &str) -> Result<(), ErrorCode> {
        let path = self.path.join(name);
        let entry = match self.open(name, DELETE | SYNCHRONIZE, FILE_SHARE_ALL, FILE_OPEN, 0) {
            Ok(entry) => entry,
            Err(status) if status.is_not_found() => return Ok(()),
            Err(status) => return Err(status.refused(&path)),
        };
        let delete = DispositionInformation { delete_file: 1 };
        set_information(&entry, &delete, FILE_DISPOSITION_INFORMATION)
            .map_err(|status| status.refused(&path))
    }

    /// A newly created file `name` holding `bytes`, written through and flushed, its
    /// handle kept open to rename it.
    fn create_file(&self, name: &str, bytes: &[u8]) -> Result<File, ErrorCode> {
        let path = self.path.join(name);
        let access = FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE;
        let mut file = self
            .open(
                name,
                access,
                0,
                FILE_CREATE,
                FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH,
            )
            .map_err(|status| status.refused(&path))?;
        file.write_all(bytes)
            .map_err(|error| failed(&path, &error))?;
        sync(&file, &path)?;
        Ok(file)
    }

    /// Renames `file`, by its handle, to `name` in this directory, replacing what is
    /// there: the entry itself, never what a link there points to.
    fn rename_into(&self, file: &File, name: &str) -> Result<(), ErrorCode> {
        let path = self.path.join(name);
        let rename =
            RenameInformation::new(&self.handle, name).map_err(|status| status.refused(&path))?;
        set_information(file, &rename, FILE_RENAME_INFORMATION)
            .map_err(|status| status.refused(&path))
    }

    fn sync(&self) -> Result<(), ErrorCode> {
        sync(&self.handle, &self.path)
    }

    /// Opens the single name `name` relative to this directory, on a reparse point
    /// itself, for synchronous I/O.
    fn open(
        &self,
        name: &str,
        access: u32,
        share: u32,
        disposition: u32,
        options: u32,
    ) -> Result<File, NtStatus> {
        let name = component(name)?;
        // `component` bounds the name below `u16::MAX` bytes.
        let bytes = (name.len() * 2) as u16;
        let object_name = UnicodeString {
            length: bytes,
            maximum_length: bytes,
            buffer: name.as_ptr().cast_mut(),
        };
        // Matches CreateFileW, which opens case-insensitively too.
        let attributes = ObjectAttributes {
            length: size_of::<ObjectAttributes>() as u32,
            root_directory: self.handle.as_raw_handle(),
            object_name: &object_name,
            attributes: OBJ_CASE_INSENSITIVE,
            security_descriptor: null(),
            security_quality_of_service: null(),
        };
        let mut handle: RawHandle = null_mut();
        let mut io = IoStatusBlock {
            status: 0,
            information: 0,
        };
        // SAFETY: every pointer is to a live local or to `self.handle`, borrowed for the
        // whole call: `handle` and `io` are writable and aligned, `attributes` points to
        // `object_name`, which points to `name`'s buffer of `bytes` bytes, never written.
        // The call is synchronous, so nothing is written to `io` after it returns. No
        // allocation size and no extended attributes are passed.
        let status = unsafe {
            NtCreateFile(
                &mut handle,
                access,
                &attributes,
                &mut io,
                null(),
                0,
                share,
                disposition,
                options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
                null(),
                0,
            )
        };
        NtStatus::check(status)?;
        // SAFETY: on success `handle` is a new, open handle nothing else owns.
        Ok(File::from(unsafe { OwnedHandle::from_raw_handle(handle) }))
    }
}

impl RenameInformation {
    /// A rename to `name` in `directory`, replacing what is there.
    fn new(directory: &File, name: &str) -> Result<Self, NtStatus> {
        let name = component(name)?;
        if name.len() > RENAME_NAME_CAPACITY {
            return Err(NtStatus(STATUS_OBJECT_NAME_INVALID));
        }
        let mut file_name = [0; RENAME_NAME_CAPACITY];
        file_name[..name.len()].copy_from_slice(&name);
        Ok(Self {
            replace_if_exists: 1,
            root_directory: directory.as_raw_handle(),
            file_name_length: (name.len() * 2) as u32,
            file_name,
        })
    }
}

/// An `NTSTATUS` a call failed with.
#[derive(Debug, Clone, Copy)]
struct NtStatus(i32);

impl NtStatus {
    /// Success and informational statuses pass, warnings and errors fail.
    fn check(status: i32) -> Result<(), Self> {
        if status >= 0 {
            Ok(())
        } else {
            Err(Self(status))
        }
    }

    fn is_not_found(self) -> bool {
        self.0 == STATUS_OBJECT_NAME_NOT_FOUND
    }

    /// Another kind of file where a directory or a file should be is `unavailable`; any
    /// other failure is `io`.
    fn refused(self, path: &Path) -> ErrorCode {
        log::error!("storage: {}: NTSTATUS {:#010x}", path.display(), self.0);
        match self.0 {
            STATUS_NOT_A_DIRECTORY | STATUS_FILE_IS_A_DIRECTORY => ErrorCode::Unavailable,
            _ => ErrorCode::Io,
        }
    }
}

/// `name` in UTF-16 when it is one entry of its directory: not empty, not `.` or `..`,
/// no separator, no stream suffix, no NUL, and short enough for a `UNICODE_STRING`.
fn component(name: &str) -> Result<Vec<u16>, NtStatus> {
    let wide: Vec<u16> = name.encode_utf16().collect();
    let single =
        !name.is_empty() && name != "." && name != ".." && !name.contains(['\\', '/', ':', '\0']);
    if !single || wide.len() * 2 > u16::MAX as usize {
        return Err(NtStatus(STATUS_OBJECT_NAME_INVALID));
    }
    Ok(wide)
}

/// Sets the information `information` of class `class` on `file`. `T` is one of the
/// `#[repr(C)]` structures above matching `class`.
fn set_information<T>(file: &File, information: &T, class: u32) -> Result<(), NtStatus> {
    let mut io = IoStatusBlock {
        status: 0,
        information: 0,
    };
    // SAFETY: `file` is an open handle borrowed for the call, `io` a writable local, and
    // `information` a live, aligned `T` of the size passed, only read. Synchronous: the
    // handle was opened for synchronous I/O, so nothing is written after the return.
    let status = unsafe {
        NtSetInformationFile(
            file.as_raw_handle(),
            &mut io,
            (information as *const T).cast(),
            size_of::<T>() as u32,
            class,
        )
    };
    NtStatus::check(status)
}

fn refuse_reparse_point(file: &File, path: &Path) -> Result<(), ErrorCode> {
    if attributes(file, path)? & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
        log::error!("storage: {} is a link", path.display());
        return Err(ErrorCode::Unavailable);
    }
    Ok(())
}

fn attributes(file: &File, path: &Path) -> Result<u32, ErrorCode> {
    file.metadata()
        .map(|metadata| metadata.file_attributes())
        .map_err(|error| failed(path, &error))
}

/// Flushes `file` to disk, its metadata included: a failure is an error, never skipped.
fn sync(file: &File, path: &Path) -> Result<(), ErrorCode> {
    file.sync_all().map_err(|error| failed(path, &error))
}

fn failed(path: &Path, error: &io::Error) -> ErrorCode {
    log::error!("storage: {}: {error}", path.display());
    ErrorCode::Io
}
