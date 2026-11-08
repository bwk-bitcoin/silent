//! The generic account layer (`src/account/`) is the account page every account kind
//! shares. It reaches neither the generated Rust header nor the lifecycle the built-in
//! controllers share (`modules/shared/AccountLifecycle.h`), except for the built-in
//! support listed below, which still lives under it. That lifecycle carries the account's
//! load, workers and connections only: the account services read coins, history,
//! transaction details and labels from the Rust account themselves.

use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    path::{Path, PathBuf},
};

const ACCOUNT_LAYER: &str = "account";
const RUST_HEADER: &str = "silent.h";
const LIFECYCLE: &str = "modules/shared/AccountLifecycle.h";
// The files of the account layer that reach the Rust header, each for its own reason.
const REACHES_RUST: &[(&str, &str)] = &[
    (
        "account/AccountModule.h",
        "the built-in modules' base, over Rust account listing",
    ),
    ("account/AccountModule.cpp", "the built-in modules' base"),
    (
        "account/ElectrumReconnect.h",
        "the built-in controllers' reconnect timing",
    ),
    (
        "account/ElectrumReconnect.cpp",
        "the built-in controllers' reconnect timing",
    ),
    (
        "account/InterfaceEnums.h",
        "maps public enums to the Rust ones",
    ),
    (
        "account/Spending.h",
        "the public IAccount spending over Rust coins",
    ),
    (
        "account/TransactionJob.h",
        "the public IAccount spending job",
    ),
    (
        "account/TransactionJob.cpp",
        "the public IAccount spending job",
    ),
    (
        "account/Money.cpp",
        "amount parsing and formatting by the Rust helpers",
    ),
    (
        "account/Services.cpp",
        "assembles through the host (host/Host.h)",
    ),
    (
        "account/services/AccountServices.h",
        "the built-in services, over the Rust account",
    ),
    (
        "account/services/ControllerServices.h",
        "the built-in services' connections",
    ),
    (
        "account/services/ControllerServices.cpp",
        "the built-in services' connections",
    ),
    (
        "account/services/Conversions.h",
        "converts the Rust types to the service ones",
    ),
    (
        "account/services/Conversions.cpp",
        "converts the Rust types to the service ones",
    ),
    (
        "account/services/ServiceAdapters.cpp",
        "the built-in restore target, over Rust",
    ),
];
// The files of the account layer that reach the built-in controllers' lifecycle.
const REACHES_LIFECYCLE: &[&str] = &[
    "account/AccountModule.cpp",
    "account/services/AccountServices.h",
    "account/services/ControllerServices.h",
    "account/services/ControllerServices.cpp",
];
// Rust types of the account's coins, history, details, labels and receiving: none is in
// the lifecycle's declarations.
const ACCOUNT_DATA_TYPES: &[&str] = &[
    "CoinState",
    "RustCoin",
    "RustTx",
    "RustTxDetails",
    "LabelEntry",
    "ReceiveAddress",
    "TransactionTemplate",
];

/// Source files by path relative to `src/`, with their contents.
type Tree = BTreeMap<String, String>;

fn src() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../src")
}

fn read_tree(dir: &Path, src: &Path, tree: &mut Tree) {
    for entry in fs::read_dir(dir).expect("read a source directory") {
        let path = entry.expect("read a source entry").path();
        if path.is_dir() {
            read_tree(&path, src, tree);
        } else if matches!(path.extension().and_then(|e| e.to_str()), Some("h" | "cpp")) {
            let relative = path
                .strip_prefix(src)
                .expect("a source under src/")
                .to_string_lossy()
                .replace('\\', "/");
            tree.insert(relative, fs::read_to_string(&path).expect("read a source"));
        }
    }
}

/// The names a source includes, quoted or angled, in order.
fn includes(source: &str) -> Vec<String> {
    source
        .lines()
        .filter_map(|line| line.trim_start().strip_prefix('#'))
        .filter_map(|line| line.trim_start().strip_prefix("include"))
        .filter_map(|line| {
            let line = line.trim();
            let close = match line.chars().next()? {
                '"' => '"',
                '<' => '>',
                _ => return None,
            };
            let name = &line[1..];
            Some(name[..name.find(close)?].to_string())
        })
        .collect()
}

/// `name` as included from `file`: next to it first, then from `src/`. None for a header
/// outside the tree (Qt, the standard library, the generated Rust header).
fn resolve(tree: &Tree, file: &str, name: &str) -> Option<String> {
    let dir = file.rfind('/').map_or("", |slash| &file[..=slash]);
    let joined = format!("{dir}{name}");
    let mut parts: Vec<&str> = Vec::new();
    for part in joined.split('/') {
        match part {
            ".." => {
                parts.pop();
            }
            "." | "" => {}
            part => parts.push(part),
        }
    }
    let beside = parts.join("/");
    if tree.contains_key(&beside) {
        return Some(beside);
    }
    tree.contains_key(name).then(|| name.to_string())
}

/// The include chain from `file` to `target` (a path in the tree, or the Rust header),
/// None when `file` does not reach it.
fn chain(tree: &Tree, file: &str, target: &str) -> Option<Vec<String>> {
    fn walk(
        tree: &Tree,
        file: &str,
        target: &str,
        seen: &mut BTreeSet<String>,
    ) -> Option<Vec<String>> {
        if !seen.insert(file.to_string()) {
            return None;
        }
        for name in includes(tree.get(file)?) {
            let next = resolve(tree, file, &name);
            if next.as_deref() == Some(target) || (next.is_none() && name == target) {
                return Some(vec![file.to_string(), target.to_string()]);
            }
            if let Some(mut rest) = next.and_then(|next| walk(tree, &next, target, seen)) {
                rest.insert(0, file.to_string());
                return Some(rest);
            }
        }
        None
    }
    walk(tree, file, target, &mut BTreeSet::new())
}

/// Every file of the account layer reaching `target` while not in `allowed`, with its
/// include chain.
fn unexpected_reaches(tree: &Tree, target: &str, allowed: &[&str]) -> Vec<String> {
    tree.keys()
        .filter(|file| file.starts_with(&format!("{ACCOUNT_LAYER}/")))
        .filter(|file| !allowed.contains(&file.as_str()))
        .filter_map(|file| chain(tree, file, target))
        .map(|chain| chain.join(" > "))
        .collect()
}

fn real_tree() -> Tree {
    let src = src();
    let mut tree = Tree::new();
    read_tree(&src, &src, &mut tree);
    tree
}

/// A declaration's words, comments left out.
fn words(source: &str) -> BTreeSet<String> {
    source
        .lines()
        .map(|line| line.split("//").next().unwrap_or_default())
        .flat_map(|line| line.split(|c: char| !c.is_alphanumeric() && c != '_'))
        .filter(|word| !word.is_empty())
        .map(str::to_string)
        .collect()
}

// ===== The Real Tree =====

#[test]
fn the_account_layer_reaches_the_rust_header_only_through_its_built_in_support() {
    let tree = real_tree();
    let allowed: Vec<&str> = REACHES_RUST.iter().map(|(file, _)| *file).collect();
    let reaches = unexpected_reaches(&tree, RUST_HEADER, &allowed);
    assert!(
        reaches.is_empty(),
        "account layer files reaching {RUST_HEADER}:\n{}",
        reaches.join("\n")
    );
}

#[test]
fn the_account_layer_reaches_the_controller_lifecycle_only_through_the_built_in_services() {
    let tree = real_tree();
    assert!(
        tree.contains_key(LIFECYCLE),
        "the built-in controllers' lifecycle is {LIFECYCLE}, on the module side"
    );
    let reaches = unexpected_reaches(&tree, LIFECYCLE, REACHES_LIFECYCLE);
    assert!(
        reaches.is_empty(),
        "account layer files reaching {LIFECYCLE}:\n{}",
        reaches.join("\n")
    );
}

#[test]
fn the_shared_account_page_reaches_no_module() {
    let tree = real_tree();
    let generic: Vec<&String> = tree
        .keys()
        .filter(|file| {
            file.starts_with("account/views/")
                || file.starts_with("account/AccountShell.")
                || file.starts_with("account/services/ServiceContext.")
                || file.as_str() == "account/services/ServiceAdapters.h"
                || file.as_str() == "account/Services.h"
        })
        .collect();
    assert!(
        generic.len() >= 20,
        "the views, the shell and the service plumbing are found, got {}",
        generic.len()
    );
    let reaches: Vec<String> = generic
        .iter()
        .flat_map(|file| {
            tree.keys()
                .filter(|module| module.starts_with("modules/"))
                .filter_map(|module| chain(&tree, file, module))
                .map(|chain| chain.join(" > "))
                .collect::<Vec<_>>()
        })
        .collect();
    assert!(
        reaches.is_empty(),
        "the shared account page reaches a module:\n{}",
        reaches.join("\n")
    );
}

#[test]
fn the_controller_lifecycle_carries_no_account_data() {
    let tree = real_tree();
    let mut found = Vec::new();
    for file in [LIFECYCLE, "modules/shared/AccountLifecycle.cpp"] {
        let source = tree
            .get(file)
            .unwrap_or_else(|| panic!("{file} is in the tree"));
        let words = words(source);
        for kind in ACCOUNT_DATA_TYPES {
            if words.contains(*kind) {
                found.push(format!("{file}: {kind}"));
            }
        }
    }
    assert!(
        found.is_empty(),
        "the controller lifecycle names the account's data:\n{}",
        found.join("\n")
    );
}

// ===== The Checker =====

fn tree_of(files: &[(&str, &str)]) -> Tree {
    files
        .iter()
        .map(|(path, source)| (path.to_string(), source.to_string()))
        .collect()
}

#[test]
fn a_reach_through_another_header_is_found_with_its_chain() {
    let tree = tree_of(&[
        (
            "account/views/Coins.cpp",
            "#include \"account/views/Coins.h\"\n",
        ),
        (
            "account/views/Coins.h",
            "#include \"Helper.h\"\n#include <QList>\n",
        ),
        ("account/views/Helper.h", "  #  include <silent.h>\n"),
        (
            "account/Clean.h",
            "// #include <silent.h>\n#include <QString>\n",
        ),
    ]);
    let reaches = unexpected_reaches(&tree, RUST_HEADER, &[]);
    assert_eq!(
        reaches,
        vec![
            "account/views/Coins.cpp > account/views/Coins.h > account/views/Helper.h > silent.h",
            "account/views/Coins.h > account/views/Helper.h > silent.h",
            "account/views/Helper.h > silent.h",
        ],
        "every file reaching the Rust header is named with its chain, a comment is not an include"
    );
    assert_eq!(
        unexpected_reaches(&tree, RUST_HEADER, &["account/views/Helper.h"]),
        vec![
            "account/views/Coins.cpp > account/views/Coins.h > account/views/Helper.h > silent.h",
            "account/views/Coins.h > account/views/Helper.h > silent.h",
        ],
        "an allowed file does not excuse the files that include it"
    );
}

#[test]
fn a_module_header_is_found_from_src() {
    let tree = tree_of(&[
        (
            "account/services/Context.h",
            "#include \"modules/shared/AccountLifecycle.h\"\n",
        ),
        (LIFECYCLE, "#include <silent.h>\n"),
    ]);
    assert_eq!(
        unexpected_reaches(&tree, LIFECYCLE, &[]),
        vec!["account/services/Context.h > modules/shared/AccountLifecycle.h"],
        "an include resolved from src/ reaches the lifecycle"
    );
}

#[test]
fn account_data_types_are_whole_words_outside_comments() {
    let found = words("void onCoins(); // CoinState arrives\nRustTxDetailsLike x;\n");
    for kind in ACCOUNT_DATA_TYPES {
        assert!(
            !found.contains(*kind),
            "{kind} in a comment or inside another name is not a use"
        );
    }
    assert!(
        words("void updateCoins(CoinState coins);").contains("CoinState"),
        "a parameter type is a use"
    );
}
