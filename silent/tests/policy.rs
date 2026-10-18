use std::str::FromStr;

use bitcoin::{
    bip32::{DerivationPath, Xpriv, Xpub},
    secp256k1::Secp256k1,
};
use silent::{
    bip87_account_path, compile_policy, preview_policy, review_policy, validate_musig_descriptor,
    Network, PolicyCompileResult, PolicyKey, PolicyKeyType, PolicyLocktime, PolicyPath,
    PolicySemantic, PolicySpec,
};

const ACCOUNT_PATH: &str = "m/87'/1'/0'";

// ===== Helpers =====

/// A key from the fixed seed `[seed; 32]`, its account xpub at `m/87'/1'/0'` (Regtest).
fn key(seed: u8, key_type: PolicyKeyType, mandatory: bool) -> PolicyKey {
    let secp = Secp256k1::new();
    let master = Xpriv::new_master(bitcoin::Network::Regtest, &[seed; 32]).unwrap();
    let account = master
        .derive_priv(&secp, &DerivationPath::from_str(ACCOUNT_PATH).unwrap())
        .unwrap();
    PolicyKey {
        fingerprint: master.fingerprint(&secp).to_string(),
        account_path: ACCOUNT_PATH.into(),
        role_xpub: Xpub::from_priv(&secp, &account).to_string(),
        key_type,
        mandatory,
    }
}

fn path(
    semantic: PolicySemantic,
    threshold: u32,
    locktime: PolicyLocktime,
    locktime_value: u32,
    keys: Vec<PolicyKey>,
) -> PolicyPath {
    PolicyPath {
        semantic,
        threshold,
        locktime,
        locktime_value,
        keys,
    }
}

fn spec(paths: Vec<PolicyPath>) -> PolicySpec {
    PolicySpec {
        network: Network::Regtest,
        paths,
    }
}

/// Primary `Single(A, Internal)`, recovery `Single(B, SafetyNet)` after 144 blocks.
fn single_key_spec() -> PolicySpec {
    spec(vec![
        path(
            PolicySemantic::Single,
            1,
            PolicyLocktime::None,
            0,
            vec![key(1, PolicyKeyType::Internal, false)],
        ),
        path(
            PolicySemantic::Single,
            1,
            PolicyLocktime::Relative,
            144,
            vec![key(2, PolicyKeyType::SafetyNet, false)],
        ),
    ])
}

/// Primary 2-of-2 `Multi{A, B}`, recovery `Single(C)` after 144 blocks.
fn two_of_two_spec() -> PolicySpec {
    spec(vec![
        path(
            PolicySemantic::Multi,
            2,
            PolicyLocktime::None,
            0,
            vec![
                key(1, PolicyKeyType::Cosigner, false),
                key(2, PolicyKeyType::Cosigner, false),
            ],
        ),
        path(
            PolicySemantic::Single,
            1,
            PolicyLocktime::Relative,
            144,
            vec![key(3, PolicyKeyType::SafetyNet, false)],
        ),
    ])
}

/// Primary 2-of-3 `Multi{A, B, C}`, recovery `Single(D)` after 1000 blocks.
fn two_of_three_spec() -> PolicySpec {
    spec(vec![
        path(
            PolicySemantic::Multi,
            2,
            PolicyLocktime::None,
            0,
            vec![
                key(1, PolicyKeyType::Cosigner, false),
                key(2, PolicyKeyType::Cosigner, false),
                key(3, PolicyKeyType::Cosigner, false),
            ],
        ),
        path(
            PolicySemantic::Single,
            1,
            PolicyLocktime::Relative,
            1000,
            vec![key(4, PolicyKeyType::SafetyNet, false)],
        ),
    ])
}

/// Compiles `spec` and asserts it is ok.
fn compiled(spec: &PolicySpec) -> PolicyCompileResult {
    let result = compile_policy(spec.clone());
    assert!(
        result.is_ok,
        "compile_policy should succeed, got error {:?}",
        result.error
    );
    result
}

/// Asserts `spec` fails with a non-empty error about `error_path`.
fn assert_path_error(spec: PolicySpec, error_path: i32) {
    let result = compile_policy(spec);
    assert!(
        !result.is_ok,
        "compile_policy should fail on path {error_path}"
    );
    assert_eq!(
        result.error_path, error_path,
        "error should name path {error_path}, got {} ({:?})",
        result.error_path, result.error
    );
    assert!(
        !result.error.is_empty(),
        "error message should not be empty"
    );
}

/// Sorted `(fingerprint, key type)` pairs, for comparing key sets across orders.
fn key_roles<'a>(keys: impl Iterator<Item = (&'a str, PolicyKeyType)>) -> Vec<(String, String)> {
    let mut roles: Vec<_> = keys
        .map(|(fingerprint, key_type)| (fingerprint.to_string(), format!("{key_type:?}")))
        .collect();
    roles.sort();
    roles
}

/// Reviews the compile of `spec` and checks it gives the spec back.
fn assert_review_round_trips(spec: &PolicySpec) {
    let result = compiled(spec);
    let review = review_policy(result.descriptor.clone());
    assert!(
        review.is_ok,
        "review_policy should succeed, got error {:?}",
        review.error
    );
    assert_eq!(
        review.paths.len(),
        spec.paths.len(),
        "review should give {} paths, got {}",
        spec.paths.len(),
        review.paths.len()
    );
    assert_eq!(
        review.leaf_count, result.leaf_count,
        "review leaf count {} should equal the compile's {}",
        review.leaf_count, result.leaf_count
    );
    for (i, (expected, reviewed)) in spec.paths.iter().zip(&review.paths).enumerate() {
        assert_eq!(
            format!("{:?}", reviewed.semantic),
            format!("{:?}", expected.semantic),
            "path {i} semantic"
        );
        assert_eq!(reviewed.threshold, expected.threshold, "path {i} threshold");
        assert_eq!(
            format!("{:?}", reviewed.locktime),
            format!("{:?}", expected.locktime),
            "path {i} locktime"
        );
        assert_eq!(
            reviewed.locktime_value, expected.locktime_value,
            "path {i} locktime value"
        );
        assert_eq!(
            key_roles(
                reviewed
                    .keys
                    .iter()
                    .map(|k| (k.fingerprint.as_str(), k.key_type))
            ),
            key_roles(
                expected
                    .keys
                    .iter()
                    .map(|k| (k.fingerprint.as_str(), k.key_type))
            ),
            "path {i} keys and key types"
        );
        let is_internal_key = i == 0 && result.has_key_path;
        assert_eq!(
            reviewed.key_path, is_internal_key,
            "path {i} key_path should be {is_internal_key}"
        );
    }
}

// ===== Account Path Tests =====

#[test]
fn bip87_account_path_per_network() {
    for (network, account, expected) in [
        (Network::Regtest, 0, "m/87'/1'/0'"),
        (Network::Bitcoin, 0, "m/87'/0'/0'"),
        (Network::Signet, 3, "m/87'/1'/3'"),
    ] {
        let path = bip87_account_path(network, account);
        assert_eq!(
            path, expected,
            "{network:?} account {account} should give {expected}, got {path}"
        );
    }
}

#[test]
fn bip87_account_path_rejects_unhardenable_account() {
    let path = bip87_account_path(Network::Regtest, 1 << 31);
    assert!(
        path.is_empty(),
        "account 2^31 does not fit a hardened index, got {path}"
    );
}

// ===== Compile Tests =====

#[test]
fn single_key_policy() {
    let spec = single_key_spec();
    let result = compiled(&spec);
    assert!(
        result.has_key_path,
        "the single primary key should be the internal key"
    );
    assert_eq!(
        result.leaf_count, 1,
        "one recovery leaf, got {}",
        result.leaf_count
    );
    assert_eq!(
        result.max_leaves, 32,
        "max leaves, got {}",
        result.max_leaves
    );
    let validated = validate_musig_descriptor(result.descriptor.clone());
    assert!(
        validated.is_ok,
        "descriptor {} should validate, got {:?}",
        result.descriptor, validated.error
    );
    assert!(
        result.descriptor.contains("older(144)"),
        "descriptor {} should hold older(144)",
        result.descriptor
    );
    let internal = format!("[{}/87'/1'/0'/0]", spec.paths[0].keys[0].fingerprint);
    let safety_net = format!("[{}/87'/1'/0'/3]", spec.paths[1].keys[0].fingerprint);
    assert!(
        result.descriptor.contains(&internal),
        "descriptor {} should hold the internal role origin {internal}",
        result.descriptor
    );
    assert!(
        result.descriptor.contains(&safety_net),
        "descriptor {} should hold the safety net role origin {safety_net}",
        result.descriptor
    );

    let mut blanked = spec.clone();
    for path in &mut blanked.paths {
        for key in &mut path.keys {
            key.role_xpub.clear();
        }
    }
    let preview = preview_policy(blanked);
    assert!(
        preview.is_ok,
        "preview_policy should succeed, got error {:?}",
        preview.error
    );
    assert_eq!(
        preview.leaf_count, result.leaf_count,
        "preview leaf count {} should equal the compile's {}",
        preview.leaf_count, result.leaf_count
    );
    assert_eq!(
        preview.has_key_path, result.has_key_path,
        "preview has_key_path should equal the compile's"
    );
    assert!(
        preview.descriptor.is_empty(),
        "preview descriptor should be empty, got {}",
        preview.descriptor
    );
}

#[test]
fn two_of_two_policy() {
    let result = compiled(&two_of_two_spec());
    assert!(
        result.has_key_path,
        "the 2-of-2 primary should be the internal musig key"
    );
    assert_eq!(
        result.leaf_count, 1,
        "one recovery leaf, got {}",
        result.leaf_count
    );
    assert!(
        result.descriptor.contains("musig("),
        "descriptor {} should hold musig(",
        result.descriptor
    );
}

#[test]
fn two_of_three_policy() {
    let result = compiled(&two_of_three_spec());
    assert!(
        !result.has_key_path,
        "a 2-of-3 primary should leave a NUMS internal key"
    );
    assert_eq!(
        result.leaf_count, 4,
        "3 musig leaves plus the recovery leaf, got {}",
        result.leaf_count
    );
    for legacy in ["multi_a", "thresh"] {
        assert!(
            !result.descriptor.contains(legacy),
            "descriptor {} should not hold {legacy}",
            result.descriptor
        );
    }
}

#[test]
fn primary_only_policy() {
    let keys = |count: u8| {
        (1..=count)
            .map(|seed| key(seed, PolicyKeyType::Cosigner, false))
            .collect::<Vec<_>>()
    };
    let two_of_three = spec(vec![path(
        PolicySemantic::Multi,
        2,
        PolicyLocktime::None,
        0,
        keys(3),
    )]);
    let result = compiled(&two_of_three);
    assert!(
        !result.has_key_path && result.leaf_count == 3,
        "a lone 2-of-3 should be 3 leaves under a NUMS key, got {} leaves, key path {}",
        result.leaf_count,
        result.has_key_path
    );
    assert_review_round_trips(&two_of_three);

    let three_of_three = spec(vec![path(
        PolicySemantic::Multi,
        3,
        PolicyLocktime::None,
        0,
        keys(3),
    )]);
    let result = compiled(&three_of_three);
    assert!(
        result.has_key_path && result.leaf_count == 0,
        "a lone 3-of-3 should be the key path alone, got {} leaves, key path {}",
        result.leaf_count,
        result.has_key_path
    );
    assert_review_round_trips(&three_of_three);

    let three_of_seven = spec(vec![path(
        PolicySemantic::Multi,
        3,
        PolicyLocktime::None,
        0,
        keys(7),
    )]);
    let result = preview_policy(three_of_seven);
    assert!(
        result.too_many_leaves && result.leaf_count == 35,
        "a lone 3-of-7 should be 35 leaves, too many, got {:?}",
        result.error
    );
}

// ===== Review Tests =====

#[test]
fn review_round_trips() {
    for spec in [single_key_spec(), two_of_two_spec(), two_of_three_spec()] {
        assert_review_round_trips(&spec);
    }
}

#[test]
fn review_rejects_garbage() {
    let review = review_policy("not a descriptor".into());
    assert!(!review.is_ok, "garbage should not review");
    assert!(
        !review.error.is_empty(),
        "error message should not be empty"
    );
    assert!(
        review.paths.is_empty(),
        "garbage should give no paths, got {}",
        review.paths.len()
    );
}

// ===== Error Handling Tests =====

#[test]
fn path_errors_name_their_path() {
    let mut bad_fingerprint = single_key_spec();
    bad_fingerprint.paths[1].keys[0].fingerprint = "not hex".into();
    assert_path_error(bad_fingerprint, 1);

    let mut relative_too_high = single_key_spec();
    relative_too_high.paths[1].locktime_value = 70000;
    assert_path_error(relative_too_high, 1);

    let mut single_with_two_keys = single_key_spec();
    single_with_two_keys.paths[0]
        .keys
        .push(key(3, PolicyKeyType::Internal, false));
    assert_path_error(single_with_two_keys, 0);

    let mut not_bip87 = single_key_spec();
    not_bip87.paths[1].keys[0].account_path = "m/84'/1'/0'".into();
    assert_path_error(not_bip87, 1);

    let mut no_role = single_key_spec();
    no_role.paths[1].keys[0].key_type = PolicyKeyType::Unknown;
    assert_path_error(no_role, 1);
}

// ===== Other Shape Tests =====

#[test]
fn three_of_seven_is_too_many_leaves() {
    let keys = (1..=7)
        .map(|seed| key(seed, PolicyKeyType::Cosigner, false))
        .collect();
    let spec = spec(vec![
        path(PolicySemantic::Multi, 3, PolicyLocktime::None, 0, keys),
        path(
            PolicySemantic::Single,
            1,
            PolicyLocktime::Relative,
            144,
            vec![key(8, PolicyKeyType::SafetyNet, false)],
        ),
    ]);
    let result = preview_policy(spec);
    assert!(!result.is_ok, "3-of-7 (35 leaves) should not preview");
    assert!(
        result.too_many_leaves,
        "error should be too many leaves, got {:?}",
        result.error
    );
    assert_eq!(
        result.leaf_count, 36,
        "3-of-7 and the recovery key need 36 leaves, got {}",
        result.leaf_count
    );
    assert_eq!(
        result.error_path, -1,
        "a policy error, got path {}",
        result.error_path
    );
    assert_eq!(
        result.max_leaves, 32,
        "max leaves, got {}",
        result.max_leaves
    );
}

#[test]
fn mandatory_key_policy() {
    let spec = spec(vec![
        path(
            PolicySemantic::Single,
            1,
            PolicyLocktime::None,
            0,
            vec![key(4, PolicyKeyType::Internal, false)],
        ),
        path(
            PolicySemantic::MultiMandatory,
            2,
            PolicyLocktime::Relative,
            144,
            vec![
                key(1, PolicyKeyType::External, true),
                key(2, PolicyKeyType::Cosigner, false),
                key(3, PolicyKeyType::Cosigner, false),
            ],
        ),
    ]);
    let result = compiled(&spec);
    assert_eq!(
        result.leaf_count, 2,
        "mandatory A with B or C gives 2 leaves, got {}",
        result.leaf_count
    );
    let review = review_policy(result.descriptor.clone());
    assert!(
        review.is_ok,
        "review_policy should succeed, got error {:?}",
        review.error
    );
    let mandatory = &review.paths[1];
    assert!(
        matches!(mandatory.semantic, PolicySemantic::MultiMandatory),
        "path 1 should review as MultiMandatory, got {:?}",
        mandatory.semantic
    );
    let mandatory_fingerprint = &spec.paths[1].keys[0].fingerprint;
    for key in &mandatory.keys {
        let expected = &key.fingerprint == mandatory_fingerprint;
        assert_eq!(
            key.mandatory, expected,
            "key {} mandatory should be {expected}",
            key.fingerprint
        );
    }
}

#[test]
fn absolute_renewable_policy() {
    let height = 1024 * 900;
    let mut spec = single_key_spec();
    spec.paths[1].locktime = PolicyLocktime::AbsoluteRenewable;
    spec.paths[1].locktime_value = height;
    let result = compiled(&spec);
    let review = review_policy(result.descriptor.clone());
    assert!(
        review.is_ok,
        "review_policy should succeed, got error {:?}",
        review.error
    );
    let recovery = &review.paths[1];
    assert!(
        matches!(recovery.locktime, PolicyLocktime::AbsoluteRenewable),
        "path 1 should review as AbsoluteRenewable, got {:?}",
        recovery.locktime
    );
    assert_eq!(
        recovery.locktime_value, height,
        "path 1 height should be {height}, got {}",
        recovery.locktime_value
    );
}

// ===== Review Key Tests =====

#[test]
fn review_keys_carry_role_origin_path() {
    let spec = single_key_spec();
    let review = review_policy(compiled(&spec).descriptor);
    assert!(
        review.is_ok,
        "review_policy should succeed, got error {:?}",
        review.error
    );
    for (i, expected) in ["m/87'/1'/0'/0", "m/87'/1'/0'/3"].into_iter().enumerate() {
        let key = &review.paths[i].keys[0];
        assert_eq!(
            key.origin_path, expected,
            "path {i} key origin path should be {expected}, got {}",
            key.origin_path
        );
        assert_eq!(
            key.fingerprint, spec.paths[i].keys[0].fingerprint,
            "path {i} key fingerprint"
        );
    }
}

// ===== Rejected Spec Tests =====

#[test]
fn empty_paths_is_a_policy_error() {
    let result = compile_policy(spec(vec![]));
    assert!(!result.is_ok, "a spec without paths should not compile");
    assert_eq!(
        result.error_path, -1,
        "a policy error, got path {}",
        result.error_path
    );
    assert!(
        !result.error.is_empty(),
        "error message should not be empty"
    );
    assert!(
        !result.too_many_leaves,
        "an empty spec is not too many leaves"
    );
}

#[test]
fn multi_threshold_out_of_range_is_rejected() {
    for threshold in [0, 4] {
        let mut spec = two_of_three_spec();
        spec.paths[0].threshold = threshold;
        let result = compile_policy(spec);
        assert!(
            !result.is_ok,
            "threshold {threshold} over 3 keys should not compile"
        );
        assert!(
            !result.error.is_empty(),
            "threshold {threshold}: error message should not be empty"
        );
        assert!(
            !result.too_many_leaves,
            "threshold {threshold}: not a too many leaves error, got {:?}",
            result.error
        );
    }
}

#[test]
fn other_semantic_is_a_path_error() {
    let mut spec = single_key_spec();
    spec.paths[1].semantic = PolicySemantic::Other;
    assert_path_error(spec, 1);
}

/// bwk-bup types this policy `Unknown` (no typed home for a mandatory key under an
/// absolute timelock), which still compiles and reviews back.
#[test]
fn mandatory_with_absolute_timelock_compiles() {
    let spec = spec(vec![
        path(
            PolicySemantic::Single,
            1,
            PolicyLocktime::None,
            0,
            vec![key(4, PolicyKeyType::Internal, false)],
        ),
        path(
            PolicySemantic::MultiMandatory,
            2,
            PolicyLocktime::AbsoluteRenewable,
            1024 * 900,
            vec![
                key(1, PolicyKeyType::External, true),
                key(2, PolicyKeyType::Cosigner, false),
                key(3, PolicyKeyType::Cosigner, false),
            ],
        ),
    ]);
    let result = compiled(&spec);
    assert_eq!(
        result.leaf_count, 2,
        "mandatory A with B or C gives 2 leaves, got {}",
        result.leaf_count
    );
    assert_review_round_trips(&spec);
}

#[test]
fn twenty_nine_of_sixty_three_is_too_many_leaves() {
    let keys = (1..=63)
        .map(|seed| key(seed, PolicyKeyType::Cosigner, false))
        .collect();
    let spec = spec(vec![
        path(PolicySemantic::Multi, 29, PolicyLocktime::None, 0, keys),
        path(
            PolicySemantic::Single,
            1,
            PolicyLocktime::Relative,
            144,
            vec![key(64, PolicyKeyType::SafetyNet, false)],
        ),
    ]);
    for result in [preview_policy(spec.clone()), compile_policy(spec)] {
        assert!(!result.is_ok, "29-of-63 should not compile");
        assert!(
            result.too_many_leaves,
            "error should be too many leaves, got {:?}",
            result.error
        );
        assert_eq!(
            result.error_path, -1,
            "a policy error, got path {}",
            result.error_path
        );
    }
}

// ===== Role xpub =====

/// `key` with its xpub at `m/87'/1'/0'/0` (the Internal role's), as keys are fetched.
fn with_role_xpub(mut key: PolicyKey, seed: u8) -> PolicyKey {
    let secp = Secp256k1::new();
    let master = Xpriv::new_master(bitcoin::Network::Regtest, &[seed; 32]).unwrap();
    let role = master
        .derive_priv(
            &secp,
            &DerivationPath::from_str(&format!("{ACCOUNT_PATH}/0")).unwrap(),
        )
        .unwrap();
    key.role_xpub = Xpub::from_priv(&secp, &role).to_string();
    key
}

#[test]
fn role_xpub_compiles_as_the_account_xpub() {
    let from_account = compiled(&two_internal_spec());
    let mut spec = two_internal_spec();
    for path in &mut spec.paths {
        for (seed, key) in (1u8..).zip(path.keys.iter_mut()) {
            *key = with_role_xpub(key.clone(), seed);
        }
    }
    let from_role = compiled(&spec);
    assert_eq!(
        from_role.descriptor, from_account.descriptor,
        "the role xpub gives the same Internal role key as the account xpub"
    );
    assert!(
        from_role.descriptor.contains("/87'/1'/0'/0]")
            || from_role.descriptor.contains("/87h/1h/0h/0]"),
        "the origin ends at R 0: {}",
        from_role.descriptor
    );
}

#[test]
fn role_xpub_refuses_another_role() {
    let mut spec = spec(vec![path(
        PolicySemantic::Single,
        1,
        PolicyLocktime::None,
        0,
        vec![with_role_xpub(key(1, PolicyKeyType::SafetyNet, false), 1)],
    )]);
    let result = compile_policy(spec.clone());
    assert!(
        !result.is_ok,
        "an Internal role xpub cannot give the Safety Net role"
    );
    spec.paths[0].keys[0] = with_role_xpub(key(1, PolicyKeyType::Internal, false), 1);
    assert!(compile_policy(spec).is_ok, "it gives the Internal role");
}

/// One 2-of-2 path, keys from seeds 1 and 2, both Internal.
fn two_internal_spec() -> PolicySpec {
    spec(vec![path(
        PolicySemantic::Multi,
        2,
        PolicyLocktime::None,
        0,
        vec![
            key(1, PolicyKeyType::Internal, false),
            key(2, PolicyKeyType::Internal, false),
        ],
    )])
}
