use crate::plugin_admission::{MAX_METADATA_BYTES, MAX_SECTIONS, NOTE_NAME, NOTE_TYPE};
use object::{
    elf, macho, pe,
    read::{
        elf::{Dyn, ElfFile, FileHeader, ProgramHeader, SectionHeader},
        macho::{MachHeader, MachOFile, Section, Segment},
        pe::{ImageNtHeaders, ImageOptionalHeader, PeFile},
    },
    Architecture, Object, ObjectSection, ObjectSegment, ObjectSymbol, SectionFlags, SectionKind,
    SegmentFlags, SymbolFlags, SymbolKind,
};

#[path = "plugin_elf_lookup.rs"]
mod elf_lookup;

const MAX_SYMBOLS: usize = 65_536;
const MAX_EXPORT_BYTES: usize = 1024 * 1024;
const REQUIRED_EXPORTS: [&[u8]; 2] = [b"qt_plugin_instance", b"qt_plugin_query_metadata_v2"];

pub(crate) fn preflight(bytes: &[u8]) -> Result<(), String> {
    match object::FileKind::parse(bytes).map_err(|e| e.to_string())? {
        object::FileKind::MachO32 => macho_counts::<macho::MachHeader32<object::Endianness>>(bytes),
        object::FileKind::MachO64 => macho_counts::<macho::MachHeader64<object::Endianness>>(bytes),
        _ => Ok(()),
    }
}

fn macho_counts<Mach: MachHeader>(bytes: &[u8]) -> Result<(), String> {
    let header = Mach::parse(bytes, 0).map_err(|e| e.to_string())?;
    let endian = header.endian().map_err(|e| e.to_string())?;
    if header.ncmds(endian) as usize > MAX_SECTIONS {
        return Err("Mach-O load commands exceed admission limit".into());
    }
    let mut commands = header
        .load_commands(endian, bytes, 0)
        .map_err(|e| e.to_string())?;
    let mut sections = 0_u64;
    while let Some(command) = commands.next().map_err(|e| e.to_string())? {
        if let Some((segment, _)) = command.segment_32().map_err(|e| e.to_string())? {
            sections += u64::from(segment.nsects(endian));
        }
        if let Some((segment, _)) = command.segment_64().map_err(|e| e.to_string())? {
            sections += u64::from(segment.nsects(endian));
        }
        if sections > MAX_SECTIONS as u64 {
            return Err("Mach-O sections exceed admission limit before allocation".into());
        }
    }
    Ok(())
}

struct ImageRange {
    offset: u64,
    size: u64,
    address: u64,
    memory_size: u64,
    read: bool,
    write: bool,
    execute: bool,
    name: Option<String>,
    mapping_granule: u64,
}

impl ImageRange {
    fn contains(&self, offset: u64, size: u64, address: u64) -> bool {
        let Some(delta) = offset.checked_sub(self.offset) else {
            return false;
        };
        delta
            .checked_add(size)
            .is_some_and(|end| end <= self.size && end <= self.memory_size)
            && self.address.checked_add(delta) == Some(address)
    }
}

pub(crate) struct NativeImage {
    ranges: Vec<ImageRange>,
    notes: Vec<QtNoteRange>,
    lookup: elf_lookup::ElfLookup,
}

struct QtNoteRange {
    offset: u64,
    address: u64,
    size: u64,
    padded_size: u64,
    read: bool,
    write: bool,
    execute: bool,
}

impl NativeImage {
    pub(crate) fn parse(file: &object::File<'_>, bytes: &[u8]) -> Result<Self, String> {
        let mut image = Self {
            ranges: Vec::new(),
            notes: Vec::new(),
            lookup: elf_lookup::ElfLookup::default(),
        };
        for (index, segment) in file.segments().enumerate() {
            if index >= MAX_SECTIONS {
                return Err("native segment count exceeds admission limit".into());
            }
            let (offset, size) = segment.file_range();
            if size == 0 && segment.size() == 0 {
                continue;
            }
            let (read, write, execute) = match segment.flags() {
                SegmentFlags::Elf { p_flags } => (
                    p_flags & elf::PF_R != 0,
                    p_flags & elf::PF_W != 0,
                    p_flags & elf::PF_X != 0,
                ),
                SegmentFlags::MachO {
                    initprot, maxprot, ..
                } => {
                    if initprot & !maxprot != 0 {
                        return Err("Mach-O segment protections exceed maximum".into());
                    }
                    (
                        initprot & macho::VM_PROT_READ != 0,
                        initprot & macho::VM_PROT_WRITE != 0,
                        initprot & macho::VM_PROT_EXECUTE != 0,
                    )
                }
                SegmentFlags::Coff { characteristics } => (
                    characteristics & pe::IMAGE_SCN_MEM_READ != 0,
                    characteristics & pe::IMAGE_SCN_MEM_WRITE != 0,
                    characteristics & pe::IMAGE_SCN_MEM_EXECUTE != 0,
                ),
                _ => return Err("unsupported native segment flags".into()),
            };
            let memory_size = if matches!(segment.flags(), SegmentFlags::Coff { .. }) {
                segment.size().max(size)
            } else {
                segment.size()
            };
            let range = ImageRange {
                offset,
                size,
                address: segment.address(),
                memory_size,
                read,
                write,
                execute,
                name: if matches!(segment.flags(), SegmentFlags::MachO { .. }) {
                    segment
                        .name()
                        .map_err(|e| e.to_string())?
                        .map(str::to_owned)
                } else {
                    None
                },
                mapping_granule: match segment.flags() {
                    SegmentFlags::Elf { .. } if file.architecture() == Architecture::Aarch64 => {
                        segment.align().max(65_536)
                    }
                    SegmentFlags::Elf { .. } => segment.align().max(4096),
                    SegmentFlags::MachO { .. } if file.architecture() == Architecture::Aarch64 => {
                        16_384
                    }
                    SegmentFlags::MachO { .. } => 4096,
                    _ => 1,
                },
            };
            validate_range(&range, bytes.len())?;
            let alignment = range.mapping_granule;
            if !matches!(segment.flags(), SegmentFlags::Coff { .. })
                && (!alignment.is_power_of_two() || offset % alignment != range.address % alignment)
            {
                return Err("inconsistent native segment alignment".into());
            }
            if matches!(segment.flags(), SegmentFlags::MachO { .. })
                && (offset % alignment != 0 || range.address % alignment != 0)
            {
                return Err("Mach-O sub-page segment mapping is unsupported".into());
            }
            for other in &image.ranges {
                if overlaps(range.offset, range.size, other.offset, other.size)
                    || overlaps(
                        range.address,
                        range.memory_size,
                        other.address,
                        other.memory_size,
                    )
                {
                    return Err("overlapping native image mappings".into());
                }
                if page_conflict(&range, other)? {
                    return Err("conflicting native page mappings".into());
                }
            }
            image.ranges.push(range);
        }
        if !image.ranges.iter().any(|range| range.size != 0) {
            return Err("native library has no file-backed image mappings".into());
        }
        match file {
            object::File::Elf32(elf) => image.elf_bindings(elf, bytes.len())?,
            object::File::Elf64(elf) => image.elf_bindings(elf, bytes.len())?,
            object::File::Pe32(pe) => validate_pe(pe, bytes.len())?,
            object::File::Pe64(pe) => validate_pe(pe, bytes.len())?,
            object::File::MachO32(mach) => image.macho_bindings(mach, bytes)?,
            object::File::MachO64(mach) => image.macho_bindings(mach, bytes)?,
            _ => {}
        }
        Ok(image)
    }

    fn macho_bindings<Mach: MachHeader>(
        &self,
        file: &MachOFile<'_, Mach>,
        bytes: &[u8],
    ) -> Result<(), String> {
        let endian = file.endian();
        let mut names = std::collections::BTreeSet::new();
        let mut count = 0;
        for segment in file.segments() {
            let raw = segment.macho_segment();
            if !names.insert(raw.name()) {
                return Err("duplicate Mach-O segment name".into());
            }
            let start = raw as *const Mach::Segment as usize - bytes.as_ptr() as usize;
            let end = start
                .checked_add(raw.cmdsize(endian) as usize)
                .ok_or("Mach-O segment command overflow")?;
            let section_data = bytes
                .get(start + std::mem::size_of::<Mach::Segment>()..end)
                .ok_or("Mach-O section headers outside load command")?;
            let sections = raw
                .sections(endian, section_data)
                .map_err(|e| e.to_string())?;
            for section in sections {
                count += 1;
                if count > MAX_SECTIONS {
                    return Err("Mach-O sections exceed admission limit".into());
                }
                if section.segname() != raw.segname() {
                    return Err("Mach-O section disagrees with declaring segment name".into());
                }
                let address: u64 = section.addr(endian).into();
                let size: u64 = section.size(endian).into();
                let delta = address
                    .checked_sub(segment.address())
                    .ok_or("Mach-O section precedes declaring segment")?;
                if delta
                    .checked_add(size)
                    .is_none_or(|end| end > segment.size())
                {
                    return Err("Mach-O section exceeds declaring segment memory".into());
                }
                let alignment = 1_u64
                    .checked_shl(section.align(endian))
                    .ok_or("Mach-O section alignment overflow")?;
                if address % alignment != 0 {
                    return Err("Mach-O section virtual alignment mismatch".into());
                }
                if matches!(
                    section.flags(endian) & macho::SECTION_TYPE,
                    macho::S_ZEROFILL | macho::S_GB_ZEROFILL | macho::S_THREAD_LOCAL_ZEROFILL
                ) {
                    continue;
                }
                let offset = u64::from(section.offset(endian));
                let (segment_offset, segment_size) = segment.file_range();
                if offset % alignment != 0
                    || segment_offset.checked_add(delta) != Some(offset)
                    || delta.checked_add(size).is_none_or(|end| end > segment_size)
                {
                    return Err(
                        "Mach-O section file offset disagrees with declaring segment mapping"
                            .into(),
                    );
                }
            }
        }
        Ok(())
    }

    fn data_at<'a>(&self, bytes: &'a [u8], address: u64) -> Result<&'a [u8], String> {
        for range in &self.ranges {
            let Some(delta) = address.checked_sub(range.address) else {
                continue;
            };
            if delta >= range.size || !range.read || range.write {
                continue;
            }
            let offset = range.offset + delta;
            let size = (range.size - delta).min(MAX_EXPORT_BYTES as u64);
            return bytes
                .get(offset as usize..(offset + size) as usize)
                .ok_or_else(|| "ELF lookup table outside mapped file".to_string());
        }
        Err("ELF lookup table is not in a read-only file-backed mapping".into())
    }

    pub(crate) fn metadata<'data>(
        &self,
        section: &impl ObjectSection<'data>,
        is_note: bool,
    ) -> Result<(), String> {
        let (offset, file_size) = section
            .file_range()
            .ok_or("metadata has no physical file range")?;
        let size = file_size.min(section.size());
        let address = section.address();
        let alignment = section.align().max(1);
        let mapped = self
            .ranges
            .iter()
            .find(|range| range.contains(offset, size, address))
            .ok_or("Qt metadata section disagrees with mapped native image")?;
        if size == 0
            || !mapped.read
            || mapped.write
            || !alignment.is_power_of_two()
            || address % alignment != 0
        {
            return Err("invalid Qt metadata mapping permissions, size or alignment".into());
        }
        match section.flags() {
            SectionFlags::Elf { sh_flags } => {
                if sh_flags & u64::from(elf::SHF_ALLOC) == 0
                    || sh_flags
                        & u64::from(elf::SHF_WRITE | elf::SHF_EXECINSTR | elf::SHF_COMPRESSED)
                        != 0
                    || offset % alignment != 0
                {
                    return Err("invalid Qt ELF metadata flags or physical alignment".into());
                }
                if is_note
                    && !self.notes.iter().any(|note| {
                        note.offset == offset
                            && note.address == address
                            && size >= note.size
                            && size <= note.padded_size
                            && note.read
                            && !note.write
                            && !note.execute
                    })
                {
                    return Err("Qt metadata section disagrees with ELF PT_NOTE location".into());
                }
            }
            SectionFlags::Coff { characteristics } => {
                if characteristics & pe::IMAGE_SCN_CNT_INITIALIZED_DATA == 0
                    || characteristics
                        & (pe::IMAGE_SCN_MEM_EXECUTE
                            | pe::IMAGE_SCN_MEM_WRITE
                            | pe::IMAGE_SCN_MEM_DISCARDABLE)
                        != 0
                {
                    return Err("invalid Qt PE metadata section flags".into());
                }
            }
            SectionFlags::MachO { flags } => {
                if mapped.name.as_deref() != Some("__TEXT")
                    || offset % alignment != 0
                    || flags
                        & (macho::SECTION_TYPE
                            | macho::S_ATTR_PURE_INSTRUCTIONS
                            | macho::S_ATTR_SOME_INSTRUCTIONS)
                        != macho::S_REGULAR
                {
                    return Err("invalid Qt Mach-O metadata section mapping or flags".into());
                }
            }
            _ => return Err("unsupported Qt metadata section flags".into()),
        }
        Ok(())
    }

    pub(crate) fn metadata_notes(&self, saw_note_section: bool) -> Result<(), String> {
        if !self.notes.is_empty() && !saw_note_section {
            return Err("orphan mapped Qt ELF metadata note".into());
        }
        Ok(())
    }

    fn elf_bindings<Elf: FileHeader>(
        &mut self,
        file: &ElfFile<'_, Elf>,
        length: usize,
    ) -> Result<(), String> {
        let endian = file.endian();
        let mut dynamic = None;
        for (index, header) in file.elf_program_headers().iter().enumerate() {
            if index >= MAX_SECTIONS {
                return Err("ELF program header count exceeds admission limit".into());
            }
            match header.p_type(endian) {
                elf::PT_NOTE => {
                    let flags = header.p_flags(endian);
                    let range = ImageRange {
                        offset: header.p_offset(endian).into(),
                        size: header.p_filesz(endian).into(),
                        address: header.p_vaddr(endian).into(),
                        memory_size: header.p_memsz(endian).into(),
                        read: flags & elf::PF_R != 0,
                        write: flags & elf::PF_W != 0,
                        execute: flags & elf::PF_X != 0,
                        name: None,
                        mapping_granule: 1,
                    };
                    validate_range(&range, length)?;
                    let alignment: u64 = header.p_align(endian).into();
                    if !matches!(alignment, 4 | 8)
                        || range.offset % alignment != 0
                        || range.address % alignment != 0
                    {
                        return Err("invalid ELF PT_NOTE alignment".into());
                    }
                    let mut notes = header
                        .notes(endian, file.data())
                        .map_err(|e| e.to_string())?
                        .ok_or("ELF PT_NOTE has no note iterator")?;
                    let mut count = 0;
                    while let Some(note) = notes.next().map_err(|e| e.to_string())? {
                        count += 1;
                        if count > MAX_SECTIONS {
                            return Err("ELF note count exceeds admission limit".into());
                        }
                        if note.name_bytes() == NOTE_NAME && note.n_type(endian) == NOTE_TYPE {
                            if !self.notes.is_empty() {
                                return Err("ambiguous mapped Qt ELF metadata notes".into());
                            }
                            if note.n_descsz(endian) as usize > MAX_METADATA_BYTES {
                                return Err(
                                    "mapped Qt ELF descriptor exceeds admission limit".into()
                                );
                            }
                            let offset = (note.name_bytes().as_ptr() as usize
                                - file.data().as_ptr() as usize
                                - 12) as u64;
                            let end = (note.desc().as_ptr() as usize
                                - file.data().as_ptr() as usize)
                                as u64
                                + u64::from(note.n_descsz(endian));
                            let size = end - offset;
                            let address = range
                                .address
                                .checked_add(offset - range.offset)
                                .ok_or("ELF note address overflow")?;
                            self.notes.push(QtNoteRange {
                                offset,
                                size,
                                address,
                                padded_size: (size + alignment - 1) & !(alignment - 1),
                                read: range.read,
                                write: range.write,
                                execute: range.execute,
                            });
                        }
                    }
                }
                elf::PT_DYNAMIC => {
                    if dynamic.is_some() {
                        return Err("ambiguous ELF PT_DYNAMIC".into());
                    }
                    let offset = header.p_offset(endian).into();
                    let size = header.p_filesz(endian).into();
                    let address = header.p_vaddr(endian).into();
                    if !self
                        .ranges
                        .iter()
                        .any(|range| range.contains(offset, size, address) && range.read)
                    {
                        return Err("ELF dynamic table is not mapped consistently".into());
                    }
                    dynamic = header
                        .dynamic(endian, file.data())
                        .map_err(|e| e.to_string())?;
                }
                _ => {}
            }
        }
        let entries = dynamic.ok_or("missing ELF dynamic table")?;
        if entries.len() > MAX_SYMBOLS {
            return Err("ELF dynamic table exceeds admission limit".into());
        }
        let mut bindings = std::collections::BTreeMap::new();
        let mut terminated = false;
        for entry in entries {
            let tag: u64 = entry.d_tag(endian).into();
            if tag == u64::from(elf::DT_NULL) {
                terminated = true;
                break;
            }
            if [
                elf::DT_SYMTAB,
                elf::DT_SYMENT,
                elf::DT_STRTAB,
                elf::DT_STRSZ,
                elf::DT_HASH,
                elf::DT_GNU_HASH,
                elf::DT_VERSYM,
            ]
            .iter()
            .any(|value| u64::from(*value) == tag)
                && bindings.insert(tag, entry.d_val(endian).into()).is_some()
            {
                return Err("duplicate ELF dynamic symbol binding".into());
            }
        }
        if !terminated {
            return Err("unterminated ELF dynamic table".into());
        }
        let table = file.elf_dynamic_symbol_table();
        if table.len() > MAX_SYMBOLS {
            return Err("ELF dynamic symbols exceed admission limit".into());
        }
        let sections = file.elf_section_table();
        for (index, tag) in [
            (table.section(), elf::DT_SYMTAB),
            (table.string_section(), elf::DT_STRTAB),
        ] {
            let section = sections.section(index).map_err(|e| e.to_string())?;
            let address = section.sh_addr(endian).into();
            let offset = section.sh_offset(endian).into();
            let size = section.sh_size(endian).into();
            let flags: u64 = section.sh_flags(endian).into();
            if flags & u64::from(elf::SHF_ALLOC) == 0
                || flags & u64::from(elf::SHF_WRITE | elf::SHF_EXECINSTR | elf::SHF_COMPRESSED) != 0
            {
                return Err("invalid ELF runtime symbol/string section flags".into());
            }
            if tag == elf::DT_STRTAB && size > MAX_EXPORT_BYTES as u64 {
                return Err("ELF dynamic strings exceed admission byte limit".into());
            }
            if bindings.get(&u64::from(tag)) != Some(&address)
                || !self
                    .ranges
                    .iter()
                    .any(|range| range.contains(offset, size, address) && range.read)
            {
                return Err("ELF symbol/string section disagrees with PT_DYNAMIC mapping".into());
            }
            if tag == elf::DT_STRTAB && bindings.get(&u64::from(elf::DT_STRSZ)) != Some(&size) {
                return Err("ELF dynamic string size disagrees with section".into());
            }
            if tag == elf::DT_SYMTAB
                && bindings.get(&u64::from(elf::DT_SYMENT))
                    != Some(&section.sh_entsize(endian).into())
            {
                return Err("ELF dynamic symbol entry size disagrees with section".into());
            }
        }
        self.lookup = elf_lookup::ElfLookup {
            gnu: bindings.get(&u64::from(elf::DT_GNU_HASH)).copied(),
            sysv: bindings.get(&u64::from(elf::DT_HASH)).copied(),
            versions: bindings.get(&u64::from(elf::DT_VERSYM)).copied(),
        };
        Ok(())
    }

    fn executable(&self, file: &object::File<'_>, address: u64, size: u64) -> Result<(), String> {
        let size = size.max(1);
        let mut matches = 0;
        for section in file.sections() {
            if section.kind() != SectionKind::Text {
                continue;
            }
            let Some(delta) = address.checked_sub(section.address()) else {
                continue;
            };
            let Some((offset, file_size)) = section.file_range() else {
                continue;
            };
            if delta
                .checked_add(size)
                .is_none_or(|end| end > section.size() || end > file_size)
            {
                continue;
            }
            if self.ranges.iter().any(|range| {
                range.read && range.execute && range.contains(offset + delta, size, address)
            }) {
                matches += 1;
            }
        }
        if matches != 1 {
            return Err(
                "Qt export is not a defined function in one mapped executable section".into(),
            );
        }
        Ok(())
    }

    pub(crate) fn exports(&self, file: &object::File<'_>, bytes: &[u8]) -> Result<(), String> {
        match file {
            object::File::Elf32(_) | object::File::Elf64(_) => self.elf_exports(file, bytes),
            object::File::Pe32(pe) => self.pe_exports(file, pe),
            object::File::Pe64(pe) => self.pe_exports(file, pe),
            object::File::MachO32(mach) => self.macho_exports(file, mach, bytes),
            object::File::MachO64(mach) => self.macho_exports(file, mach, bytes),
            _ => Err("unsupported native export format".into()),
        }
    }

    fn elf_exports(&self, file: &object::File<'_>, bytes: &[u8]) -> Result<(), String> {
        let mut found = [false; 2];
        let mut name_bytes = 0;
        for (index, symbol) in file.dynamic_symbols().enumerate() {
            if index >= MAX_SYMBOLS {
                return Err("ELF dynamic symbols exceed admission limit".into());
            }
            let name = symbol.name_bytes().map_err(|e| e.to_string())?;
            name_bytes += name.len();
            if name_bytes > MAX_EXPORT_BYTES {
                return Err("ELF export names exceed admission byte limit".into());
            }
            let Some(required) = REQUIRED_EXPORTS
                .iter()
                .position(|expected| *expected == name)
            else {
                continue;
            };
            if found[required]
                || !symbol.is_definition()
                || !symbol.is_global()
                || symbol.kind() != SymbolKind::Text
                || !matches!(symbol.flags(), SymbolFlags::Elf { st_info, st_other } if st_info & 0xf == elf::STT_FUNC && matches!(st_other & 3, elf::STV_DEFAULT | elf::STV_PROTECTED))
            {
                return Err(format!(
                    "Qt entry {} is not one defined exported function",
                    String::from_utf8_lossy(name)
                ));
            }
            let section = file
                .section_by_index(
                    symbol
                        .section_index()
                        .ok_or("Qt entry has no defining section")?,
                )
                .map_err(|e| e.to_string())?;
            let delta = symbol
                .address()
                .checked_sub(section.address())
                .ok_or("Qt entry precedes defining section")?;
            if section.kind() != SectionKind::Text
                || delta
                    .checked_add(symbol.size().max(1))
                    .is_none_or(|end| end > section.size())
            {
                return Err("Qt entry disagrees with its defining executable section".into());
            }
            self.executable(file, symbol.address(), symbol.size())?;
            self.lookup
                .validate(self, bytes, file, symbol.index().0, name)?;
            found[required] = true;
        }
        require_exports(found)
    }

    fn pe_exports<Pe: ImageNtHeaders>(
        &self,
        file: &object::File<'_>,
        pe: &PeFile<'_, Pe>,
    ) -> Result<(), String> {
        let table = pe
            .export_table()
            .map_err(|e| e.to_string())?
            .ok_or("missing PE export table")?;
        if table.name_pointers().len() > MAX_SYMBOLS || table.addresses().len() > MAX_SYMBOLS {
            return Err("PE exports exceed admission limit".into());
        }
        let mut found = [false; 2];
        let mut previous = None;
        let mut name_bytes = 0;
        for (pointer, index) in table.name_iter() {
            let name = table
                .name_from_pointer(pointer)
                .map_err(|e| e.to_string())?;
            name_bytes += name.len();
            if name_bytes > MAX_EXPORT_BYTES {
                return Err("PE export names exceed admission byte limit".into());
            }
            if previous.is_some_and(|previous| previous >= name) {
                return Err("PE export names are not uniquely sorted for runtime lookup".into());
            }
            previous = Some(name);
            let Some(required) = REQUIRED_EXPORTS
                .iter()
                .position(|expected| *expected == name)
            else {
                continue;
            };
            let address = table
                .address_by_index(index.into())
                .map_err(|e| e.to_string())?;
            if table.is_forward(address) {
                return Err("forwarded Qt PE entry exports are unsupported".into());
            }
            if found[required] || address == 0 {
                return Err("ambiguous or null Qt PE entry export".into());
            }
            self.executable(
                file,
                file.relative_address_base()
                    .checked_add(address.into())
                    .ok_or("PE export address overflow")?,
                1,
            )?;
            found[required] = true;
        }
        require_exports(found)
    }

    fn macho_exports<Mach: MachHeader>(
        &self,
        file: &object::File<'_>,
        mach: &MachOFile<'_, Mach>,
        bytes: &[u8],
    ) -> Result<(), String> {
        let endian = mach.endian();
        let mut commands = mach.macho_load_commands().map_err(|e| e.to_string())?;
        let mut export_range = None;
        let mut count = 0;
        while let Some(command) = commands.next().map_err(|e| e.to_string())? {
            count += 1;
            if count > MAX_SECTIONS {
                return Err("Mach-O load commands exceed admission limit".into());
            }
            let range = if command.cmd() == macho::LC_DYLD_EXPORTS_TRIE {
                let data: &macho::LinkeditDataCommand<Mach::Endian> =
                    command.data().map_err(|e| e.to_string())?;
                Some((data.dataoff.get(endian), data.datasize.get(endian)))
            } else if let Some(data) = command.dyld_info().map_err(|e| e.to_string())? {
                (data.export_size.get(endian) != 0)
                    .then_some((data.export_off.get(endian), data.export_size.get(endian)))
            } else {
                None
            };
            if let Some(range) = range {
                if export_range.replace(range).is_some() {
                    return Err("ambiguous Mach-O export tries".into());
                }
            }
        }
        let (offset, size) =
            export_range.ok_or("Mach-O without a dyld export trie is unsupported")?;
        if size as usize > MAX_EXPORT_BYTES || size == 0 {
            return Err("invalid Mach-O export trie size".into());
        }
        if !self.ranges.iter().any(|range| {
            range.name.as_deref() == Some("__LINKEDIT")
                && range.read
                && !range.write
                && u64::from(offset) >= range.offset
                && u64::from(offset)
                    .checked_add(size.into())
                    .is_some_and(|end| end <= range.offset + range.size)
        }) {
            return Err("Mach-O export trie is outside mapped __LINKEDIT".into());
        }
        let data = bytes
            .get(offset as usize..offset as usize + size as usize)
            .ok_or("Mach-O export trie outside file")?;
        let base = self
            .ranges
            .iter()
            .find(|range| range.offset == 0 && range.name.as_deref() == Some("__TEXT"))
            .ok_or("missing Mach-O __TEXT image base")?
            .address;
        for name in REQUIRED_EXPORTS {
            let mut mangled = vec![b'_'];
            mangled.extend(name);
            let relative = trie_export(data, &mangled)?;
            self.executable(
                file,
                base.checked_add(relative)
                    .ok_or("Mach-O export address overflow")?,
                1,
            )?;
        }
        Ok(())
    }
}

fn require_exports(found: [bool; 2]) -> Result<(), String> {
    for (index, present) in found.iter().enumerate() {
        if !present {
            return Err(format!(
                "missing defined Qt entry export {}",
                String::from_utf8_lossy(REQUIRED_EXPORTS[index])
            ));
        }
    }
    Ok(())
}

fn validate_range(range: &ImageRange, length: usize) -> Result<(), String> {
    if range.size > range.memory_size
        || range
            .offset
            .checked_add(range.size)
            .is_none_or(|end| end > length as u64)
        || range.address.checked_add(range.memory_size).is_none()
    {
        return Err("native mapped image range exceeds file or virtual bounds".into());
    }
    Ok(())
}

fn overlaps(start: u64, size: u64, other: u64, other_size: u64) -> bool {
    size != 0 && other_size != 0 && start < other + other_size && other < start + size
}

fn page_conflict(range: &ImageRange, other: &ImageRange) -> Result<bool, String> {
    let granule = range.mapping_granule.max(other.mapping_granule);
    if granule == 1 || range.memory_size == 0 || other.memory_size == 0 {
        return Ok(false);
    }
    let end = range
        .address
        .checked_add(range.memory_size)
        .and_then(|end| end.checked_add(granule - 1))
        .ok_or("native page mapping overflow")?
        & !(granule - 1);
    let other_end = other
        .address
        .checked_add(other.memory_size)
        .and_then(|end| end.checked_add(granule - 1))
        .ok_or("native page mapping overflow")?
        & !(granule - 1);
    let start = range.address & !(granule - 1);
    let other_start = other.address & !(granule - 1);
    if !overlaps(start, end - start, other_start, other_end - other_start) {
        return Ok(false);
    }
    Ok(range.size == 0
        || other.size == 0
        || i128::from(range.address) - i128::from(range.offset)
            != i128::from(other.address) - i128::from(other.offset)
        || range.read != other.read
        || range.write != other.write
        || range.execute != other.execute)
}

fn validate_pe<Pe: ImageNtHeaders>(file: &PeFile<'_, Pe>, length: usize) -> Result<(), String> {
    let optional = file.nt_headers().optional_header();
    let alignment = u64::from(optional.section_alignment());
    let file_alignment = u64::from(optional.file_alignment());
    if alignment < 4096 {
        return Err("PE sub-page section alignment is unsupported".into());
    }
    let headers = u64::from(optional.size_of_headers());
    let image = u64::from(optional.size_of_image());
    let sections = file.section_table();
    let minimum_headers = u64::from(file.dos_header().nt_headers_offset())
        + 24
        + u64::from(
            file.nt_headers()
                .file_header()
                .size_of_optional_header
                .get(object::LittleEndian),
        )
        + sections.len() as u64 * 40;
    if !alignment.is_power_of_two()
        || !file_alignment.is_power_of_two()
        || alignment < file_alignment
        || (alignment < 4096 && alignment != file_alignment)
        || (alignment >= 4096 && !(512..=65_536).contains(&file_alignment))
        || headers < minimum_headers
        || headers > length as u64
        || headers > image
        || optional.image_base().checked_add(image).is_none()
        || image % alignment != 0
        || headers % file_alignment != 0
    {
        return Err("invalid PE image/header alignment or bounds".into());
    }
    let mut ranges = Vec::new();
    for section in sections.iter() {
        let offset = u64::from(section.pointer_to_raw_data.get(object::LittleEndian));
        let size = u64::from(section.size_of_raw_data.get(object::LittleEndian));
        let rva = u64::from(section.virtual_address.get(object::LittleEndian));
        let memory_size = u64::from(section.virtual_size.get(object::LittleEndian)).max(size);
        if (size != 0
            && (offset < headers
                || offset % file_alignment != 0
                || size % file_alignment != 0
                || offset
                    .checked_add(size)
                    .is_none_or(|end| end > length as u64)))
            || rva < headers
            || rva % alignment != 0
            || rva.checked_add(memory_size).is_none_or(|end| end > image)
        {
            return Err(
                "PE section mapping exceeds image or disagrees with header alignment".into(),
            );
        }
        if ranges
            .iter()
            .any(|&(other_offset, other_size, other_rva, other_memory)| {
                overlaps(offset, size, other_offset, other_size)
                    || overlaps(rva, memory_size, other_rva, other_memory)
            })
        {
            return Err("overlapping PE physical or virtual sections".into());
        }
        ranges.push((offset, size, rva, memory_size));
    }
    Ok(())
}

fn uleb(data: &[u8], offset: &mut usize) -> Result<u64, String> {
    let mut result = 0;
    for shift in (0..70).step_by(7) {
        let byte = *data.get(*offset).ok_or("truncated Mach-O export ULEB")?;
        *offset += 1;
        if shift == 63 && byte > 1 {
            return Err("Mach-O export ULEB overflow".into());
        }
        result |= u64::from(byte & 0x7f) << shift;
        if byte & 0x80 == 0 {
            return Ok(result);
        }
    }
    Err("Mach-O export ULEB overflow".into())
}

fn trie_export(data: &[u8], name: &[u8]) -> Result<u64, String> {
    let mut offset = 0;
    let mut matched = 0;
    let mut visited = std::collections::BTreeSet::new();
    for _ in 0..128 {
        if !visited.insert(offset) {
            return Err("cyclic Mach-O export trie".into());
        }
        let terminal_size = usize::try_from(uleb(data, &mut offset)?).map_err(|e| e.to_string())?;
        let terminal_end = offset
            .checked_add(terminal_size)
            .filter(|end| *end <= data.len())
            .ok_or("Mach-O export terminal outside trie")?;
        if matched == name.len() && terminal_size != 0 {
            let terminal = &data[..terminal_end];
            let flags = uleb(terminal, &mut offset)?;
            if flags & !u64::from(macho::EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION) != 0 {
                return Err(
                    "Mach-O Qt reexport, resolver or non-regular export is unsupported".into(),
                );
            }
            let address = uleb(terminal, &mut offset)?;
            if offset != terminal_end {
                return Err("malformed Mach-O export terminal".into());
            }
            return Ok(address);
        }
        offset = terminal_end;
        let count = *data.get(offset).ok_or("missing Mach-O export children")?;
        offset += 1;
        let mut next = None;
        for _ in 0..count {
            let start = offset;
            let length = data
                .get(start..)
                .and_then(|rest| rest.iter().position(|byte| *byte == 0))
                .ok_or("unterminated Mach-O export edge")?;
            if length == 0 {
                return Err("empty Mach-O export edge".into());
            }
            offset += length + 1;
            let target = usize::try_from(uleb(data, &mut offset)?).map_err(|e| e.to_string())?;
            if name[matched..].starts_with(&data[start..start + length])
                && next.replace((target, matched + length)).is_some()
            {
                return Err("ambiguous Mach-O export edges".into());
            }
        }
        (offset, matched) = next.ok_or_else(|| {
            format!(
                "missing defined Mach-O Qt entry export {}",
                String::from_utf8_lossy(name)
            )
        })?;
    }
    Err("Mach-O export trie depth exceeds admission limit".into())
}
