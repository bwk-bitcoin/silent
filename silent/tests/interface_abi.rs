//! A public interface header whose declarations change must raise the IID it is
//! admitted under. `src/interfaces/abi_hashes.txt` keeps, append-only, one row per
//! layout of each header: the IIDs the header declares, the root plugin IID it shipped
//! under and the hash of its declarations.

use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
};

use bitcoin::hashes::{sha256, Hash};

const TABLE: &str = "interfaces/abi_hashes.txt";
const PLUGIN_HEADER: &str = "interfaces/module.h";
const PLUGIN_PREFIX: &str = "dev.silent.IPlugin/";
const IID_PREFIX: &str = "dev.silent.";
// Stands in for an IID literal, so raising one IID leaves the other rows of a header
// with several IIDs (module.h) unchanged.
const IID_MASK: &str = "\"IID\"";
// Types an interface header carries across the boundary from a header outside
// `interfaces/`: (interface header, defining header, declaration opening). Only that
// declaration, up to its closing `};`, joins the interface header's layout.
const BOUNDARY_TYPES: &[(&str, &str, &str)] =
    &[("interfaces/theme.h", "theme/Palette.h", "struct Palette {")];

#[derive(Debug, Clone, PartialEq)]
struct Row {
    header: String,
    iids: Vec<String>,
    plugin: String,
    hash: String,
}

#[derive(Debug, Clone)]
struct Header {
    path: String,
    iids: Vec<String>,
    hash: String,
}

/// The header's declarations, without comments, IID literals or layout whitespace.
fn declarations(source: &str) -> String {
    let mut out = String::new();
    let mut chars = source.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '/' if chars.peek() == Some(&'/') => {
                for c in chars.by_ref() {
                    if c == '\n' {
                        out.push('\n');
                        break;
                    }
                }
            }
            '/' if chars.peek() == Some(&'*') => {
                chars.next();
                let mut previous = ' ';
                for c in chars.by_ref() {
                    if previous == '*' && c == '/' {
                        break;
                    }
                    previous = c;
                }
                out.push(' ');
            }
            '"' | '\'' => {
                let literal = quoted(c, &mut chars);
                if literal.starts_with(&format!("{c}{IID_PREFIX}")) {
                    out.push_str(IID_MASK);
                } else {
                    out.push_str(&literal);
                }
            }
            c => out.push(c),
        }
    }
    out.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// The literal opened by `quote`, quotes and escapes included.
fn quoted(quote: char, chars: &mut impl Iterator<Item = char>) -> String {
    let mut literal = String::from(quote);
    while let Some(c) = chars.next() {
        literal.push(c);
        match c {
            '\\' => literal.extend(chars.next()),
            c if c == quote => break,
            _ => {}
        }
    }
    literal
}

/// The IIDs the header declares, sorted.
fn declared_iids(source: &str) -> Vec<String> {
    let needle = format!("\"{IID_PREFIX}");
    let mut iids: Vec<String> = source
        .match_indices(&needle)
        .filter_map(|(start, _)| {
            let rest = &source[start + 1..];
            rest.find('"').map(|end| rest[..end].to_string())
        })
        .collect();
    iids.sort();
    iids.dedup();
    iids
}

fn parse_header(path: &str, source: &str) -> Header {
    let declarations = declarations(source);
    Header {
        path: path.to_string(),
        iids: declared_iids(source),
        hash: sha256::Hash::hash(declarations.as_bytes()).to_string(),
    }
}

fn parse_table(table: &str) -> Vec<Row> {
    table
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .map(|line| {
            let fields: Vec<&str> = line.split_whitespace().collect();
            assert_eq!(
                fields.len(),
                4,
                "{TABLE}: `{line}` is not `header iids plugin_iid hash`"
            );
            let iids = match fields[1] {
                "-" => Vec::new(),
                iids => iids.split(',').map(str::to_string).collect(),
            };
            Row {
                header: fields[0].to_string(),
                iids,
                plugin: fields[2].to_string(),
                hash: fields[3].to_string(),
            }
        })
        .collect()
}

/// `dev.silent.IPlugin/11.0` as (11, 0).
fn plugin_version(iid: &str) -> (u32, u32) {
    let version = iid
        .strip_prefix(PLUGIN_PREFIX)
        .unwrap_or_else(|| panic!("{iid} is not a {PLUGIN_PREFIX} IID"));
    let (major, minor) = version
        .split_once('.')
        .unwrap_or_else(|| panic!("{iid} has no major.minor version"));
    (
        major
            .parse()
            .unwrap_or_else(|_| panic!("{iid} has a non-numeric major")),
        minor
            .parse()
            .unwrap_or_else(|_| panic!("{iid} has a non-numeric minor")),
    )
}

fn table_line(header: &Header, plugin: &str) -> String {
    let iids = if header.iids.is_empty() {
        "-".to_string()
    } else {
        header.iids.join(",")
    };
    format!("{} {} {} {}", header.path, iids, plugin, header.hash)
}

/// What to raise for a change in `header`: its own IIDs and always the root one.
fn to_raise(header: &Header, plugin: &str) -> String {
    let mut iids: Vec<&str> = header
        .iids
        .iter()
        .map(String::as_str)
        .filter(|iid| !iid.starts_with(PLUGIN_PREFIX))
        .collect();
    iids.push(plugin);
    iids.join(" and ")
}

/// Every way the headers disagree with the table, as messages for the author.
fn check(headers: &[Header], rows: &[Row]) -> Vec<String> {
    let mut errors = Vec::new();
    let plugin = headers
        .iter()
        .find(|header| header.path == PLUGIN_HEADER)
        .and_then(|header| {
            header
                .iids
                .iter()
                .find(|iid| iid.starts_with(PLUGIN_PREFIX))
        })
        .expect("module.h declares the root plugin IID")
        .clone();

    for header in headers {
        let history: Vec<&Row> = rows
            .iter()
            .filter(|row| row.header == header.path)
            .collect();
        let Some(last) = history.last() else {
            errors.push(format!(
                "{} is not in {TABLE}: append `{}`",
                header.path,
                table_line(header, &plugin)
            ));
            continue;
        };
        if last.hash != header.hash {
            errors.push(format!(
                "{} changed since its last row in {TABLE}: raise {} and append `{}`",
                header.path,
                to_raise(header, &plugin),
                table_line(header, &plugin)
            ));
        } else if last.iids != header.iids {
            errors.push(format!(
                "{} declares {:?} but its last row in {TABLE} names {:?}: append `{}`",
                header.path,
                header.iids,
                last.iids,
                table_line(header, &plugin)
            ));
        }
        if plugin_version(&last.plugin) > plugin_version(&plugin) {
            errors.push(format!(
                "{}: its last row in {TABLE} names {} but module.h declares {plugin}",
                header.path, last.plugin
            ));
        }
        errors.extend(one_layout_per_iid(&header.path, &history));
    }
    errors
}

/// An IID names one layout of a header: a second hash under it means it was not raised.
fn one_layout_per_iid(path: &str, history: &[&Row]) -> Vec<String> {
    let mut layouts: BTreeMap<&str, &str> = BTreeMap::new();
    let mut errors = Vec::new();
    for row in history {
        for iid in row.iids.iter().chain([&row.plugin]) {
            match layouts.insert(iid, &row.hash) {
                Some(hash) if hash != row.hash => errors.push(format!(
                    "{path}: {iid} names two layouts in {TABLE}: raise {iid} for the newer one"
                )),
                _ => {}
            }
        }
    }
    errors.dedup();
    errors
}

fn headers_in(dir: &Path, src: &Path, headers: &mut Vec<Header>) {
    for entry in fs::read_dir(dir).expect("list the interfaces directory") {
        let path = entry.expect("read an interfaces entry").path();
        if path.is_dir() {
            headers_in(&path, src, headers);
        } else if path.extension().is_some_and(|extension| extension == "h") {
            let mut source = fs::read_to_string(&path).expect("read an interface header");
            let relative = path.strip_prefix(src).expect("header under src");
            let relative = relative.to_string_lossy();
            for (header, defining, opening) in BOUNDARY_TYPES {
                if relative == *header {
                    let defining = fs::read_to_string(src.join(defining))
                        .expect("read a boundary type's header");
                    source.push_str(boundary_type(&defining, opening));
                }
            }
            headers.push(parse_header(&relative, &source));
        }
    }
}

/// The declaration of `source` that starts at `opening`, through its closing `};`.
fn boundary_type<'a>(source: &'a str, opening: &str) -> &'a str {
    let start = source
        .find(opening)
        .unwrap_or_else(|| panic!("boundary type `{opening}` not found"));
    let length = source[start..]
        .find("\n};")
        .unwrap_or_else(|| panic!("boundary type `{opening}` not closed"));
    &source[start..start + length + 3]
}

fn src() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../src")
}

#[test]
fn interface_headers_match_their_iids() {
    let src = src();
    let mut headers = Vec::new();
    headers_in(&src.join("interfaces"), &src, &mut headers);
    headers.sort_by(|a, b| a.path.cmp(&b.path));
    let rows = parse_table(&fs::read_to_string(src.join(TABLE)).expect("read the ABI table"));
    let errors = check(&headers, &rows);
    assert!(
        errors.is_empty(),
        "public interface layout changed:\n{}",
        errors.join("\n")
    );
}

const KEYS_V3: &str = r#"
class IKeys : public QObject {
    virtual void sign() = 0; // signs
};
Q_DECLARE_INTERFACE(IKeys, "dev.silent.IKeys/3.0")
"#;
const KEYS_V3_COMMENTED: &str = r#"
class IKeys : public QObject {
    /* Signs with the key. */
    virtual void sign() = 0;
};
Q_DECLARE_INTERFACE(IKeys, "dev.silent.IKeys/3.0")
"#;
const KEYS_V3_CHANGED: &str = r#"
class IKeys : public QObject {
    virtual void prepareNonces() = 0;
    virtual void sign() = 0;
};
Q_DECLARE_INTERFACE(IKeys, "dev.silent.IKeys/3.0")
"#;
const KEYS_V4: &str = r#"
class IKeys : public QObject {
    virtual void prepareNonces() = 0;
    virtual void sign() = 0;
};
Q_DECLARE_INTERFACE(IKeys, "dev.silent.IKeys/4.0")
"#;
const TYPES: &str = "struct Coin { int value; };";
const TYPES_CHANGED: &str = "struct Coin { int value; int height; };";
const ROOT_11: &str = r#"
class IPlugin { virtual void modules() = 0; };
#define SILENT_IPlugin_IID "dev.silent.IPlugin/11.0"
#define SILENT_IModule_IID "dev.silent.IModule/3.0"
"#;
const ROOT_12: &str = r#"
class IPlugin { virtual void modules() = 0; };
#define SILENT_IPlugin_IID "dev.silent.IPlugin/12.0"
#define SILENT_IModule_IID "dev.silent.IModule/3.0"
"#;

fn sdk(root: &str, keys: &str, types: &str) -> Vec<Header> {
    vec![
        parse_header("interfaces/module.h", root),
        parse_header("interfaces/keys.h", keys),
        parse_header("interfaces/types.h", types),
    ]
}

/// The table rows of `headers`, all shipped under `plugin`.
fn rows_of(headers: &[Header], plugin: &str) -> Vec<Row> {
    headers
        .iter()
        .map(|header| parse_table(&table_line(header, plugin)).remove(0))
        .collect()
}

#[test]
fn unchanged_headers_pass() {
    let headers = sdk(ROOT_11, KEYS_V3, TYPES);
    let rows = rows_of(&headers, "dev.silent.IPlugin/11.0");
    assert_eq!(check(&headers, &rows), Vec::<String>::new());
}

#[test]
fn comments_and_whitespace_are_not_layout() {
    let rows = rows_of(&sdk(ROOT_11, KEYS_V3, TYPES), "dev.silent.IPlugin/11.0");
    let headers = sdk(ROOT_11, KEYS_V3_COMMENTED, TYPES);
    assert_eq!(check(&headers, &rows), Vec::<String>::new());
}

#[test]
fn a_change_without_a_raise_names_the_header_and_its_iids() {
    let rows = rows_of(&sdk(ROOT_11, KEYS_V3, TYPES), "dev.silent.IPlugin/11.0");
    let headers = sdk(ROOT_11, KEYS_V3_CHANGED, TYPES);
    let errors = check(&headers, &rows);
    assert_eq!(errors.len(), 1, "{errors:?}");
    assert!(
        errors[0].starts_with(
            "interfaces/keys.h changed since its last row in interfaces/abi_hashes.txt: \
             raise dev.silent.IKeys/3.0 and dev.silent.IPlugin/11.0 and append `"
        ),
        "{}",
        errors[0]
    );
}

#[test]
fn a_shared_header_change_names_the_root_iid() {
    let rows = rows_of(&sdk(ROOT_11, KEYS_V3, TYPES), "dev.silent.IPlugin/11.0");
    let headers = sdk(ROOT_11, KEYS_V3, TYPES_CHANGED);
    let errors = check(&headers, &rows);
    assert_eq!(errors.len(), 1, "{errors:?}");
    assert!(
        errors[0].starts_with(
            "interfaces/types.h changed since its last row in interfaces/abi_hashes.txt: \
             raise dev.silent.IPlugin/11.0 and append `"
        ),
        "{}",
        errors[0]
    );
}

#[test]
fn a_row_appended_without_raising_the_own_iid_fails() {
    let mut rows = rows_of(&sdk(ROOT_11, KEYS_V3, TYPES), "dev.silent.IPlugin/11.0");
    let headers = sdk(ROOT_12, KEYS_V3_CHANGED, TYPES);
    rows.extend(rows_of(&headers[..2], "dev.silent.IPlugin/12.0"));
    assert_eq!(
        check(&headers, &rows),
        vec![
            "interfaces/keys.h: dev.silent.IKeys/3.0 names two layouts in \
             interfaces/abi_hashes.txt: raise dev.silent.IKeys/3.0 for the newer one"
                .to_string()
        ]
    );
}

#[test]
fn a_row_appended_without_raising_the_root_iid_fails() {
    let mut rows = rows_of(&sdk(ROOT_11, KEYS_V3, TYPES), "dev.silent.IPlugin/11.0");
    let headers = sdk(ROOT_11, KEYS_V4, TYPES);
    rows.push(rows_of(&headers[1..2], "dev.silent.IPlugin/11.0").remove(0));
    assert_eq!(
        check(&headers, &rows),
        vec![
            "interfaces/keys.h: dev.silent.IPlugin/11.0 names two layouts in \
             interfaces/abi_hashes.txt: raise dev.silent.IPlugin/11.0 for the newer one"
                .to_string()
        ]
    );
}

#[test]
fn a_raise_of_both_iids_with_its_row_passes() {
    let mut rows = rows_of(&sdk(ROOT_11, KEYS_V3, TYPES), "dev.silent.IPlugin/11.0");
    let headers = sdk(ROOT_12, KEYS_V4, TYPES);
    rows.extend(rows_of(&headers[..2], "dev.silent.IPlugin/12.0"));
    assert_eq!(check(&headers, &rows), Vec::<String>::new());
}

#[test]
fn a_raised_iid_without_its_row_fails() {
    let rows = rows_of(&sdk(ROOT_11, KEYS_V3, TYPES), "dev.silent.IPlugin/11.0");
    let headers = sdk(ROOT_12, KEYS_V3, TYPES);
    let errors = check(&headers, &rows);
    assert_eq!(errors.len(), 1, "{errors:?}");
    assert!(
        errors[0].starts_with(
            "interfaces/module.h declares [\"dev.silent.IModule/3.0\", \"dev.silent.IPlugin/12.0\"] \
             but its last row in interfaces/abi_hashes.txt names \
             [\"dev.silent.IModule/3.0\", \"dev.silent.IPlugin/11.0\"]: append `"
        ),
        "{}",
        errors[0]
    );
}

#[test]
fn a_header_missing_from_the_table_fails() {
    let headers = sdk(ROOT_11, KEYS_V3, TYPES);
    let rows = rows_of(&headers[..2], "dev.silent.IPlugin/11.0");
    let errors = check(&headers, &rows);
    assert_eq!(errors.len(), 1, "{errors:?}");
    assert!(
        errors[0].starts_with("interfaces/types.h is not in interfaces/abi_hashes.txt: append `"),
        "{}",
        errors[0]
    );
}

#[test]
fn a_boundary_type_is_its_declaration_only() {
    let defining = "namespace color {\nconst int x = 1;\n}\n\nstruct Palette {\n    QColor bg;\n};\n\nconst int y = 2;\n";
    assert_eq!(
        boundary_type(defining, "struct Palette {"),
        "struct Palette {\n    QColor bg;\n};"
    );
}
