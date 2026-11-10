use crate::plugin_admission::{inspect, Compatibility};
use serde_cbor::Value;
use std::collections::BTreeMap;

fn host(os: &str) -> Compatibility {
    Compatibility {
        iid: "dev.silent.IPlugin/1.0".into(),
        target_os: os.into(),
        target_arch: "x86_64".into(),
        qt_major: 6,
        qt_minor: 8,
        arch_requirements: 1,
    }
}

fn record() -> Vec<u8> {
    let root = BTreeMap::from([(Value::Integer(2), Value::Text(host("linux").iid))]);
    let mut bytes = b"QTMETADATA !\x01\x06\x06\x01".to_vec();
    bytes.extend(serde_cbor::to_vec(&root).expect("encode metadata fixture"));
    bytes
}

fn put(bytes: &mut [u8], offset: usize, value: u64, width: usize) {
    bytes[offset..offset + width].copy_from_slice(&value.to_le_bytes()[..width]);
}

fn gnu_table(bloom_size: usize, bucket_count: usize) -> Vec<u8> {
    let mut table = vec![0; 16 + bloom_size * 8 + bucket_count * 4 + 8];
    for (offset, value) in [(0, bucket_count), (4, 1), (8, bloom_size), (12, 6)] {
        put(&mut table, offset, value as u64, 4);
    }
    table[16..16 + bloom_size * 8].fill(0xff);
    let buckets = 16 + bloom_size * 8;
    let chains = buckets + bucket_count * 4;
    // These two Qt export hashes share a bucket for counts 1, 2 and 3.
    put(
        &mut table,
        buckets + (2_827_130_156_usize % bucket_count) * 4,
        1,
        4,
    );
    put(&mut table, chains, 2_827_130_156 & !1, 4);
    put(&mut table, chains + 4, 3_438_164_180 | 1, 4);
    table
}

fn elf_gnu(table: &[u8]) -> Vec<u8> {
    let mut bytes = elf(&record());
    put(&mut bytes, 1312, 0x6fff_fef5, 8);
    put(&mut bytes, 1320, 800, 8);
    bytes[800..800 + table.len()].copy_from_slice(table);
    bytes
}

#[test]
fn gnu_hash_header_bounds_and_valid_controls() {
    for bloom_size in [1, 2, 4, 8] {
        for buckets in [1, 2, 3] {
            for shift in [0, 6, 31] {
                let mut table = gnu_table(bloom_size, buckets);
                put(&mut table, 12, shift, 4);
                assert!(
                    inspect(&elf_gnu(&table), &host("linux"), None).is_ok(),
                    "valid GNU bloom {bloom_size}, buckets {buckets}, shift {shift}"
                );
            }
        }
    }
    for (offset, value) in [
        (0, 0),
        (0, 65537),
        (4, 0),
        (4, 4),
        (4, u32::MAX as u64),
        (8, 0),
        (8, 3),
        (8, 5),
        (8, u32::MAX as u64),
        (12, 32),
        (12, 63),
        (12, 64),
        (12, u32::MAX as u64),
    ] {
        let mut table = gnu_table(4, 1);
        put(&mut table, offset, value, 4);
        let bytes = elf_gnu(&table);
        let outcome = std::panic::catch_unwind(|| inspect(&bytes, &host("linux"), None));
        assert!(
            outcome
                .expect("malformed GNU hash must not unwind")
                .expect_err("invalid GNU header must reject")
                .contains("GNU export hash bounds"),
            "GNU field {offset}={value} must be rejected"
        );
        let boundary = std::panic::catch_unwind(|| {
            crate::plugin_admission::plugin_admission(&bytes, host("linux").iid, 6, 8, 1)
        });
        assert!(
            boundary
                .expect("Rust/CXX admission function must not panic")
                .contains("GNU export hash bounds"),
            "GNU field {offset}={value} must return an error string at the bridge"
        );
    }
}

#[test]
fn gnu_hash_all_buckets_and_complete_chains_are_checked() {
    for value in [1, 2, 3, u32::MAX as u64] {
        let mut table = gnu_table(4, 2);
        put(&mut table, 52, value, 4);
        refuse(&elf_gnu(&table), "linux", "GNU export hash bucket");
    }
    let mut unterminated = gnu_table(4, 1);
    put(&mut unterminated, 56, 3_438_164_180 & !1, 4);
    refuse(
        &elf_gnu(&unterminated),
        "linux",
        "unterminated ELF GNU export hash chain",
    );
    let mut split = gnu_table(4, 1);
    put(&mut split, 52, 2_827_130_156 | 1, 4);
    refuse(
        &elf_gnu(&split),
        "linux",
        "split ELF GNU export hash bucket chain",
    );
    let mut incongruent = gnu_table(4, 1);
    put(&mut incongruent, 56, (3_438_164_180 ^ 2) | 1, 4);
    refuse(
        &elf_gnu(&incongruent),
        "linux",
        "incongruent ELF GNU export hash",
    );
    let mut bloom = gnu_table(4, 1);
    bloom[16..48].fill(0);
    refuse(&elf_gnu(&bloom), "linux", "runtime export hash bloom");
}

fn elf(record: &[u8]) -> Vec<u8> {
    let names = b"\0.shstrtab\0.qtmetadata\0.dynsym\0.dynstr\0.text\0.dynamic\0";
    let strings = b"\0qt_plugin_instance\0qt_plugin_query_metadata_v2\0";
    let mut bytes = vec![0; 1360];
    bytes[..7].copy_from_slice(b"\x7fELF\x02\x01\x01");
    put(&mut bytes, 16, 3, 2);
    put(&mut bytes, 18, 62, 2);
    put(&mut bytes, 20, 1, 4);
    put(&mut bytes, 32, 64, 8);
    put(&mut bytes, 40, 232, 8);
    put(&mut bytes, 52, 64, 2);
    put(&mut bytes, 54, 56, 2);
    put(&mut bytes, 56, 3, 2);
    put(&mut bytes, 58, 64, 2);
    put(&mut bytes, 60, 7, 2);
    put(&mut bytes, 62, 1, 2);
    put(&mut bytes, 64, 1, 4);
    put(&mut bytes, 68, 5, 4);
    put(&mut bytes, 96, (1360 + record.len()) as u64, 8);
    put(&mut bytes, 104, (1360 + record.len()) as u64, 8);
    put(&mut bytes, 112, 4096, 8);
    put(&mut bytes, 120, 2, 4);
    put(&mut bytes, 124, 4, 4);
    put(&mut bytes, 128, 1248, 8);
    put(&mut bytes, 136, 1248, 8);
    put(&mut bytes, 152, 96, 8);
    put(&mut bytes, 160, 96, 8);
    put(&mut bytes, 168, 8, 8);
    for (index, name, kind, offset, size, flags) in [
        (1, 1, 3, 680, names.len(), 0),
        (2, 11, 1, 1360, record.len(), 2),
        (3, 23, 11, 1152, 72, 2),
        (4, 31, 3, 1024, strings.len(), 2),
        (5, 39, 1, 1344, 2, 6),
        (6, 45, 6, 1248, 96, 2),
    ] {
        let base = 232 + index * 64;
        put(&mut bytes, base, name, 4);
        put(&mut bytes, base + 4, kind, 4);
        put(&mut bytes, base + 8, flags, 8);
        if flags != 0 {
            put(&mut bytes, base + 16, offset, 8);
        }
        put(&mut bytes, base + 24, offset, 8);
        put(&mut bytes, base + 32, size as u64, 8);
        put(&mut bytes, base + 48, 1, 8);
    }
    put(&mut bytes, 232 + 3 * 64 + 40, 4, 4);
    put(&mut bytes, 232 + 3 * 64 + 56, 24, 8);
    put(&mut bytes, 232 + 6 * 64 + 40, 4, 4);
    put(&mut bytes, 232 + 6 * 64 + 56, 16, 8);
    bytes[680..680 + names.len()].copy_from_slice(names);
    bytes[1024..1024 + strings.len()].copy_from_slice(strings);
    for (index, value) in [1, 3, 1, 0, 2, 0].into_iter().enumerate() {
        put(&mut bytes, 960 + index * 4, value, 4);
    }
    for (index, name, address) in [(1, 1, 1344), (2, 20, 1345)] {
        let base = 1152 + index * 24;
        put(&mut bytes, base, name, 4);
        bytes[base + 4] = 0x12;
        put(&mut bytes, base + 6, 5, 2);
        put(&mut bytes, base + 8, address, 8);
        put(&mut bytes, base + 16, 1, 8);
    }
    for (index, tag, value) in [
        (0, 6, 1152),
        (1, 11, 24),
        (2, 5, 1024),
        (3, 10, strings.len() as u64),
        (4, 4, 960),
    ] {
        put(&mut bytes, 1248 + index * 16, tag, 8);
        put(&mut bytes, 1256 + index * 16, value, 8);
    }
    bytes[1344..1346].copy_from_slice(&[0xc3, 0xc3]);
    bytes.extend(record);
    bytes
}

fn pe(record: &[u8]) -> Vec<u8> {
    let mut bytes = vec![0; 2048];
    bytes[..2].copy_from_slice(b"MZ");
    put(&mut bytes, 60, 64, 4);
    bytes[64..68].copy_from_slice(b"PE\0\0");
    put(&mut bytes, 68, 0x8664, 2);
    put(&mut bytes, 70, 3, 2);
    put(&mut bytes, 84, 240, 2);
    put(&mut bytes, 86, 0x2002, 2);
    put(&mut bytes, 88, 0x20b, 2);
    put(&mut bytes, 112, 0x0001_8000_0000, 8);
    put(&mut bytes, 120, 4096, 4);
    put(&mut bytes, 124, 512, 4);
    put(&mut bytes, 144, 0x4000, 4);
    put(&mut bytes, 148, 512, 4);
    put(&mut bytes, 196, 16, 4);
    put(&mut bytes, 200, 0x3000, 4);
    put(&mut bytes, 204, 256, 4);
    for (index, name, size, rva, offset, flags) in [
        (
            0,
            b".qtmetad".as_slice(),
            record.len(),
            0x1000,
            512,
            0x4000_0040,
        ),
        (1, b".text".as_slice(), 2, 0x2000, 1024, 0x6000_0020),
        (2, b".edata".as_slice(), 256, 0x3000, 1536, 0x4000_0040),
    ] {
        let base = 328 + index * 40;
        bytes[base..base + name.len()].copy_from_slice(name);
        put(&mut bytes, base + 8, size as u64, 4);
        put(&mut bytes, base + 12, rva, 4);
        put(&mut bytes, base + 16, 512, 4);
        put(&mut bytes, base + 20, offset, 4);
        put(&mut bytes, base + 36, flags, 4);
    }
    bytes[512..512 + record.len()].copy_from_slice(record);
    bytes[1024..1026].copy_from_slice(&[0xc3, 0xc3]);
    for (offset, value) in [
        (16, 1),
        (20, 2),
        (24, 2),
        (28, 0x3028),
        (32, 0x3030),
        (36, 0x3038),
        (40, 0x2000),
        (44, 0x2001),
        (48, 0x3050),
        (52, 0x3063),
    ] {
        put(&mut bytes, 1536 + offset, value, 4);
    }
    put(&mut bytes, 1536 + 58, 1, 2);
    let names = b"qt_plugin_instance\0qt_plugin_query_metadata_v2\0";
    bytes[1616..1616 + names.len()].copy_from_slice(names);
    bytes
}

fn elf_note(record: &[u8]) -> Vec<u8> {
    let mut note = vec![0; 24];
    put(&mut note, 0, 12, 4);
    put(&mut note, 4, (record.len() - 12) as u64, 4);
    put(&mut note, 8, 0x7451_0001, 4);
    note[12..24].copy_from_slice(b"qt-project!\0");
    note.extend(&record[12..]);
    let mut bytes = elf(&note);
    bytes[734..752].copy_from_slice(b".note.qt.metadata\0");
    put(&mut bytes, 328, 72, 8);
    put(&mut bytes, 360, 54, 4);
    put(&mut bytes, 364, 7, 4);
    put(&mut bytes, 408, 8, 8);
    for (offset, value, width) in [
        (176, 4, 4),
        (180, 4, 4),
        (184, 1360, 8),
        (192, 1360, 8),
        (208, note.len() as u64, 8),
        (216, note.len() as u64, 8),
        (224, 8, 8),
    ] {
        put(&mut bytes, offset, value, width);
    }
    bytes
}

fn refuse(bytes: &[u8], os: &str, fragment: &str) {
    let error =
        inspect(bytes, &host(os), None).expect_err("invalid native image must fail admission");
    assert!(
        error.contains(fragment),
        "expected refusal {fragment:?} on {os}, found {error}"
    );
}

#[test]
fn native_metadata_must_match_physical_image_mappings() {
    let record = record();
    let original = elf(&record);
    let mut overlay = original.clone();
    put(&mut overlay, 384, original.len() as u64, 8);
    overlay.extend(&record);
    refuse(&overlay, "linux", "mapped native image");
    let mut page_alias = original.clone();
    page_alias.resize(6160, 0);
    for (offset, value, width) in [
        (176, 1, 4),
        (180, 5, 4),
        (184, 6144, 8),
        (192, 2048, 8),
        (208, 16, 8),
        (216, 16, 8),
        (224, 4096, 8),
    ] {
        put(&mut page_alias, offset, value, width);
    }
    refuse(&page_alias, "linux", "page mappings");
    for (offset, value) in [(368, 0), (368, 3), (368, 6), (376, 1368), (408, 3)] {
        let mut bytes = original.clone();
        put(&mut bytes, offset, value, 8);
        refuse(&bytes, "linux", "metadata");
    }
    let mut note = elf_note(&record);
    assert!(
        inspect(&note, &host("linux"), None).is_ok(),
        "mapped Qt ELF note is admitted"
    );
    put(&mut note, 176, 0, 4);
    refuse(&note, "linux", "PT_NOTE location");

    let original = pe(&record);
    let mut subpage = original.clone();
    put(&mut subpage, 120, 512, 4);
    refuse(&subpage, "windows", "sub-page section alignment");
    for (offset, value) in [
        (348, 513),
        (340, 0x4000),
        (348, 1536),
        (364, 0xc000_0040),
        (144, 0x2000),
    ] {
        let mut bytes = original.clone();
        put(&mut bytes, offset, value, 4);
        assert!(
            inspect(&bytes, &host("windows"), None).is_err(),
            "PE physical/virtual mapping field {offset}={value} must be refused"
        );
    }
    let original = macho(&record);
    let mut partial_last_page = original.clone();
    put(
        &mut partial_last_page,
        296,
        (original.len() - 4096) as u64,
        8,
    );
    assert!(
        inspect(&partial_last_page, &host("macos"), None).is_ok(),
        "Mach-O permits an unrounded final __LINKEDIT size"
    );
    let mut too_many = original.clone();
    put(&mut too_many, 16, 4097, 4);
    refuse(&too_many, "macos", "load commands exceed admission limit");
    put(&mut too_many, 16, 3, 4);
    put(&mut too_many, 96, 4097, 4);
    refuse(&too_many, "macos", "before allocation");
    let mut overlay = original.clone();
    put(&mut overlay, 152, original.len() as u64, 4);
    overlay.extend(&record);
    refuse(&overlay, "macos", "declaring segment mapping");
    let mut wrong_segment = original.clone();
    wrong_segment[120..126].copy_from_slice(b"__DATA");
    refuse(&wrong_segment, "macos", "declaring segment name");
    let mut wrong_address = original;
    put(&mut wrong_address, 136, 0x0001_0000_0408, 8);
    refuse(&wrong_address, "macos", "declaring segment mapping");
}

#[test]
fn nested_note_bytes_are_not_a_pt_note_record_boundary() {
    let inner_file = elf_note(&record());
    let inner_note = &inner_file[1360..];
    for padding in 0..8 {
        let mut blob = vec![0; padding];
        blob.extend(inner_note);
        let root = BTreeMap::from([
            (
                Value::Integer(2),
                Value::Text("dev.silent.IPlugin/7.0".into()),
            ),
            (Value::Integer(4), Value::Bytes(blob)),
        ]);
        let mut regular = record()[..16].to_vec();
        regular.extend(serde_cbor::to_vec(&root).expect("nested ELF note fixture"));
        let mut bytes = elf_note(&regular);
        let offset = bytes
            .windows(inner_note.len())
            .position(|part| part == inner_note)
            .expect("embedded note bytes");
        if offset % 8 != 0 {
            continue;
        }
        put(&mut bytes, 376, offset as u64, 8);
        put(&mut bytes, 384, offset as u64, 8);
        put(&mut bytes, 392, inner_note.len() as u64, 8);
        refuse(&bytes, "linux", "PT_NOTE location");
        return;
    }
    panic!("nested note fixture must have an aligned location");
}

#[test]
fn required_exports_use_runtime_tables_and_executable_definitions() {
    let original = elf(&record());
    for (offset, value, width) in [
        (1025, b'x' as u64, 1),
        (1044, b'x' as u64, 1),
        (1176 + 6, 0, 2),
        (1176 + 4, 0x11, 1),
        (1176 + 5, 2, 1),
        (1176 + 8, 1360, 8),
        (392 + 64, 0, 8),
        (1312 + 8, 1, 8),
        (968, 0, 4),
    ] {
        let mut bytes = original.clone();
        put(&mut bytes, offset, value, width);
        assert!(
            inspect(&bytes, &host("linux"), None).is_err(),
            "invalid ELF export binding field {offset}={value} must reject"
        );
    }
    let mut section_overlay = original.clone();
    let symbols = original[1152..1224].to_vec();
    put(&mut section_overlay, 448, original.len() as u64, 8);
    section_overlay.extend(symbols);
    refuse(&section_overlay, "linux", "PT_DYNAMIC");

    let original = pe(&record());
    let mut forwarded = original.clone();
    put(&mut forwarded, 1576, 0x3050, 4);
    refuse(&forwarded, "windows", "forwarded Qt PE");
    let mut data_export = original.clone();
    put(&mut data_export, 1576, 0x1000, 4);
    refuse(&data_export, "windows", "defined function");
    let mut no_exports = original.clone();
    put(&mut no_exports, 200, 0, 4);
    refuse(&no_exports, "windows", "missing PE export table");
    let mut unordered_names = original;
    put(&mut unordered_names, 1584, 0x3063, 4);
    put(&mut unordered_names, 1588, 0x3050, 4);
    refuse(&unordered_names, "windows", "not uniquely sorted");

    let original = macho(&record());
    let mut resolver = original.clone();
    resolver[4096 + 47] = 0x10;
    refuse(&resolver, "macos", "resolver");
    let mut reexport = original.clone();
    reexport[4096 + 47] = 8;
    refuse(&reexport, "macos", "reexport");
    let mut cyclic = original.clone();
    cyclic[4096 + 14] = 0;
    refuse(&cyclic, "macos", "cyclic");
    let mut data_export = original.clone();
    data_export[4096 + 49] = 8;
    refuse(&data_export, "macos", "defined function");
    let mut overlay = original.clone();
    let trie = original[4096..].to_vec();
    put(&mut overlay, 344, original.len() as u64, 4);
    overlay.extend(trie);
    refuse(&overlay, "macos", "outside mapped __LINKEDIT");
}

#[test]
fn duplicate_keys_at_every_depth_and_global_cbor_budgets() {
    let iid = &record()[17..];
    for (root_size, suffix) in [
        (0xa3, "04a004a0"),
        (0xa3, "0400180401"),
        (0xa3, "616e01616e02"),
        (0xa2, "04a2617801617802"),
        (0xa2, "04a201010102"),
        (0xa2, "04a1616181a2617801617802"),
        (0xa2, "04c0a2617801617802"),
        (0xa2, "04a1a261780161780200"),
    ] {
        let mut bytes = record()[..16].to_vec();
        bytes.push(root_size);
        bytes.extend(iid);
        bytes.extend(hex::decode(suffix).expect("duplicate CBOR fixture"));
        refuse(&elf(&bytes), "linux", "duplicate CBOR map key");
    }
    for (header, repeated, ending) in [
        (vec![0x9f], vec![0; 20_000], vec![0xff]),
        (vec![0x9f], vec![0xa0; 20_000], vec![0xff]),
        (vec![0x99, 0x4e, 0x20], Vec::new(), Vec::new()),
        (vec![0xb9, 0x4e, 0x20], Vec::new(), Vec::new()),
    ] {
        let mut bytes = record()[..16].to_vec();
        bytes.extend([0xa2]);
        bytes.extend(iid);
        bytes.push(4);
        bytes.extend(header);
        bytes.extend(repeated);
        bytes.extend(ending);
        refuse(&elf(&bytes), "linux", "budget");
    }
    assert!(
        crate::plugin_admission::cbor::read(&vec![0; 1024 * 1024 + 1])
            .err()
            .expect("oversized CBOR must reject")
            .contains("byte budget"),
        "CBOR input byte budget is enforced before decoding"
    );
}

fn macho(record: &[u8]) -> Vec<u8> {
    let mut bytes = vec![0; 4096];
    put(&mut bytes, 0, 0xfeed_facf, 4);
    put(&mut bytes, 4, 0x0100_0007, 4);
    put(&mut bytes, 8, 3, 4);
    put(&mut bytes, 12, 6, 4);
    put(&mut bytes, 16, 3, 4);
    put(&mut bytes, 20, 320, 4);
    put(&mut bytes, 32, 0x19, 4);
    put(&mut bytes, 36, 232, 4);
    bytes[40..46].copy_from_slice(b"__TEXT");
    put(&mut bytes, 56, 0x0001_0000_0000, 8);
    put(&mut bytes, 64, 4096, 8);
    put(&mut bytes, 80, 4096, 8);
    put(&mut bytes, 88, 5, 4);
    put(&mut bytes, 92, 5, 4);
    put(&mut bytes, 96, 2, 4);
    bytes[104..114].copy_from_slice(b"qtmetadata");
    bytes[120..126].copy_from_slice(b"__TEXT");
    put(&mut bytes, 136, 0x0001_0000_0400, 8);
    put(&mut bytes, 144, record.len() as u64, 8);
    put(&mut bytes, 152, 1024, 4);
    bytes[184..190].copy_from_slice(b"__text");
    bytes[200..206].copy_from_slice(b"__TEXT");
    put(&mut bytes, 216, 0x0001_0000_0200, 8);
    put(&mut bytes, 224, 2, 8);
    put(&mut bytes, 232, 512, 4);
    put(&mut bytes, 248, 0x8000_0400, 4);
    put(&mut bytes, 264, 0x19, 4);
    put(&mut bytes, 268, 72, 4);
    bytes[272..282].copy_from_slice(b"__LINKEDIT");
    put(&mut bytes, 288, 0x0001_0000_1000, 8);
    put(&mut bytes, 296, 4096, 8);
    put(&mut bytes, 304, 4096, 8);
    let trie = b"\0\x01_qt_plugin_\0\x0f\0\x02instance\0\x2equery_metadata_v2\0\x33\x03\0\x80\x04\0\x03\0\x81\x04\0";
    put(&mut bytes, 312, trie.len() as u64, 8);
    put(&mut bytes, 320, 1, 4);
    put(&mut bytes, 324, 1, 4);
    put(&mut bytes, 336, 0x8000_0033, 4);
    put(&mut bytes, 340, 16, 4);
    put(&mut bytes, 344, 4096, 4);
    put(&mut bytes, 348, trie.len() as u64, 4);
    bytes[512..514].copy_from_slice(&[0xc3, 0xc3]);
    bytes[1024..1024 + record.len()].copy_from_slice(record);
    bytes.extend(trie);
    bytes
}

#[test]
fn native_format_contracts_and_manifest_agreement() {
    let payload = record();
    for (os, bytes) in [
        ("linux", elf(&payload)),
        ("windows", pe(&payload)),
        ("macos", macho(&payload)),
    ] {
        let host = host(os);
        let found = inspect(&bytes, &host, None).expect("native format fixture admission");
        assert_eq!(
            found.qt_minor, 6,
            "metadata uses plugin Qt minor, not host minor"
        );
        assert_eq!(
            inspect(&bytes, &host, Some(&found)),
            Ok(found.clone()),
            "agreeing manifest is admitted on {os}"
        );
        for field in 0..6 {
            let mut manifest = found.clone();
            match field {
                0 => manifest.iid.push_str("extra"),
                1 => manifest.target_os = "other".into(),
                2 => manifest.target_arch = "aarch64".into(),
                3 => manifest.qt_major = 5,
                4 => manifest.qt_minor = 7,
                _ => manifest.arch_requirements = 0x81,
            }
            assert!(
                inspect(&bytes, &host, Some(&manifest))
                    .expect_err("manifest disagreement")
                    .contains("manifest/library"),
                "manifest field {field} must agree on {os}"
            );
        }
        let mut orphan = bytes.clone();
        // Removing the metadata bytes leaves its declared native range truncated.
        orphan.truncate(orphan.len() - payload.len());
        assert!(
            inspect(&orphan, &host, None).is_err(),
            "truncated section is refused on {os}"
        );
        let mut hidden = bytes.clone();
        let marker: &[u8] = if os == "macos" {
            b"qtmetadata"
        } else if os == "windows" {
            b".qtmetad"
        } else {
            b".qtmetadata"
        };
        let offset = hidden
            .windows(marker.len())
            .position(|part| part == marker)
            .expect("metadata section name");
        hidden[offset] = b'X';
        hidden.extend(&payload);
        assert!(
            inspect(&hidden, &host, None).is_err(),
            "orphan and appended current metadata cannot rescue missing native section on {os}"
        );
        assert!(
            inspect(&payload, &host, None).is_err(),
            "raw records are not native libraries on {os}"
        );
    }
}

#[test]
fn metadata_build_requirements_and_resources() {
    let linux_host = host("linux");
    for (offset, value) in [
        (12, 0),
        (12, 2),
        (13, 5),
        (13, 7),
        (14, 9),
        (15, 2),
        (15, 0x7f),
    ] {
        let mut payload = record();
        payload[offset] = value;
        assert!(
            inspect(&elf(&payload), &linux_host, None).is_err(),
            "metadata byte {offset}={value} must be rejected"
        );
    }
    let mut payload = record();
    payload[15] = 0x81;
    assert!(
        inspect(&pe(&payload), &host("windows"), None).is_err(),
        "Windows debug mismatch is refused"
    );
    assert!(
        inspect(&elf(&payload), &linux_host, None).is_ok(),
        "Unix permits debug/release mixing like Qt"
    );
    let mut outside = elf(&record());
    put(&mut outside, 384, u64::MAX, 8);
    assert!(
        inspect(&outside, &linux_host, None).is_err(),
        "overflowing section offset is refused"
    );
    put(&mut outside, 384, 1360, 8);
    put(&mut outside, 392, 1024 * 1024 + 1, 8);
    assert!(
        inspect(&outside, &linux_host, None).is_err(),
        "metadata resource limit is enforced before CBOR"
    );
    let mut malicious_cbor = record()[..16].to_vec();
    malicious_cbor.extend(b"\xa1\x04\x9b\xff\xff\xff\xff\xff\xff\xff\xff");
    assert!(
        inspect(&elf(&malicious_cbor), &linux_host, None).is_err(),
        "huge CBOR collection length is refused"
    );
}

#[test]
fn universal_macho_selects_one_bounded_host_slice() {
    let thin = macho(&record());
    let mut fat = vec![0; 32];
    fat[..4].copy_from_slice(&0xcafe_babe_u32.to_be_bytes());
    fat[4..8].copy_from_slice(&1_u32.to_be_bytes());
    fat[8..12].copy_from_slice(&0x0100_0007_u32.to_be_bytes());
    fat[12..16].copy_from_slice(&3_u32.to_be_bytes());
    fat[16..20].copy_from_slice(&32_u32.to_be_bytes());
    fat[20..24].copy_from_slice(&(thin.len() as u32).to_be_bytes());
    fat.extend(&thin);
    assert!(
        inspect(&fat, &host("macos"), None).is_ok(),
        "bounded universal host slice is admitted"
    );
    fat[20..24].copy_from_slice(&u32::MAX.to_be_bytes());
    assert!(
        inspect(&fat, &host("macos"), None).is_err(),
        "out-of-bounds universal slice is refused"
    );
    fat[20..24].copy_from_slice(&(thin.len() as u32).to_be_bytes());
    fat[8..12].copy_from_slice(&0x0100_000c_u32.to_be_bytes());
    assert!(
        inspect(&fat, &host("macos"), None).is_err(),
        "non-host universal slice cannot satisfy admission"
    );
}

#[test]
fn tagged_root_or_iid_and_excessive_depth_are_not_qt_metadata() {
    let valid = record();
    let mut tagged_root = valid[..16].to_vec();
    tagged_root.extend([0xc0]);
    tagged_root.extend(&valid[16..]);
    let tagged_iid = BTreeMap::from([(
        Value::Integer(2),
        Value::Tag(0, Box::new(Value::Text(host("linux").iid))),
    )]);
    let mut tagged_string = valid[..16].to_vec();
    tagged_string.extend(serde_cbor::to_vec(&tagged_iid).expect("tagged IID fixture"));
    let mut deep = valid[..16].to_vec();
    deep.extend([0xa1, 4]);
    deep.extend(vec![0x81; 140]);
    deep.push(0);
    for bytes in [tagged_root, tagged_string, deep] {
        assert!(
            inspect(&elf(&bytes), &host("linux"), None).is_err(),
            "tagged or excessively nested metadata must be rejected"
        );
    }
}

#[test]
#[ignore = "requires SILENT_ABI_FIXTURE_PATH pointing to the compiled ABI 1 fixture"]
fn compiled_manifest_mismatch_does_not_execute() {
    let path = std::env::var("SILENT_ABI_FIXTURE_PATH").expect("compiled fixture path is required");
    let bytes = std::fs::read(path).expect("read compiled native fixture");
    let directory = temp_dir::TempDir::new().expect("sentinel directory");
    let sentinel = directory.path().join("must-not-execute");
    let previous = std::env::var_os("SILENT_ABI_SENTINEL");
    std::env::set_var("SILENT_ABI_SENTINEL", &sentinel);
    let mut host = host(std::env::consts::OS);
    host.target_arch = std::env::consts::ARCH.into();
    let found = inspect(&bytes, &host, None).expect("compiled ABI 1 fixture admission");
    let mut manifest = found.clone();
    manifest.iid = "dev.silent.IPlugin/13.0".into();
    let rejected = inspect(&bytes, &host, Some(&manifest));
    let executed = sentinel.exists();
    match previous {
        Some(value) => std::env::set_var("SILENT_ABI_SENTINEL", value),
        None => std::env::remove_var("SILENT_ABI_SENTINEL"),
    }
    assert!(
        rejected
            .expect_err("manifest mismatch must reject")
            .contains("manifest/library"),
        "compiled library must agree with manifest"
    );
    assert!(
        !executed,
        "inspection and manifest refusal must run neither initializer nor constructor"
    );
}
