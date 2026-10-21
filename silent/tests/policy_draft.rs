use silent::{
    policy_duration_blocks, policy_threshold, renewable_height, resolve_policy_draft, Network,
    PolicyDraft, PolicyDraftError, PolicyDurationUnit, PolicyLocktime, PolicySemantic,
};

fn draft(locktime: PolicyLocktime) -> PolicyDraft {
    PolicyDraft {
        keys: vec!["11111111".into(), "22222222".into(), "33333333".into()],
        mandatory: vec!["11111111".into()],
        threshold: 1,
        locktime,
        count: 1,
        unit: PolicyDurationUnit::Days,
        date_timestamp: 1_700_000_601,
        date_valid: true,
    }
}

#[test]
fn threshold_semantics_and_duration_units() {
    for picked in 0..=6 {
        for mandatory in 0..=picked {
            for threshold in 0..=8 {
                let resolved = policy_threshold(picked, mandatory, threshold);
                let highest = picked.max(1);
                assert_eq!(
                    resolved,
                    threshold.max((mandatory + 1).min(highest)).min(highest),
                    "normalization for {picked}/{mandatory}/{threshold}"
                );
            }
        }
    }
    for (unit, expected) in [
        (PolicyDurationUnit::Blocks, 2),
        (PolicyDurationUnit::Days, 288),
        (PolicyDurationUnit::Weeks, 2016),
        (PolicyDurationUnit::Months, 8640),
    ] {
        assert_eq!(
            policy_duration_blocks(2, unit),
            expected,
            "duration unit {unit:?}"
        );
    }
    let resolved = resolve_policy_draft(
        draft(PolicyLocktime::None),
        Network::Regtest,
        false,
        0,
        1_700_000_000,
    );
    assert!(resolved.is_ok, "mandatory draft: {}", resolved.error);
    assert_eq!(resolved.path.threshold, 2, "mandatory plus one optional");
    assert_eq!(
        resolved.path.semantic,
        PolicySemantic::MultiMandatory,
        "mandatory semantics"
    );
    let mut single = draft(PolicyLocktime::None);
    single.keys.truncate(1);
    single.threshold = 6;
    let resolved = resolve_policy_draft(single, Network::Regtest, false, 0, 0);
    assert!(resolved.is_ok, "single draft: {}", resolved.error);
    assert_eq!(
        resolved.path.semantic,
        PolicySemantic::Single,
        "single semantics"
    );
    assert_eq!(resolved.path.threshold, 1, "single threshold");
}

#[test]
fn deterministic_lock_resolution_and_limits() {
    let resolve = |draft| resolve_policy_draft(draft, Network::Regtest, true, 1000, 1_700_000_000);
    let date = resolve(draft(PolicyLocktime::Absolute));
    assert!(date.is_ok, "future date: {}", date.error);
    assert_eq!(
        date.path.locktime_value, 1002,
        "601 seconds rounds up to two blocks"
    );
    let renewable = resolve(draft(PolicyLocktime::AbsoluteRenewable));
    assert!(renewable.is_ok, "renewable: {}", renewable.error);
    assert_eq!(
        renewable.path.locktime_value,
        renewable_height(1144),
        "height grid"
    );
    let mut relative = draft(PolicyLocktime::Relative);
    relative.unit = PolicyDurationUnit::Blocks;
    relative.count = 65535;
    assert!(
        resolve(relative.clone()).is_ok,
        "relative boundary accepted"
    );
    relative.count = 65536;
    assert_eq!(
        resolve(relative).lock_error,
        PolicyDraftError::RelativeMax,
        "relative bound"
    );
    for kind in [PolicyLocktime::Relative, PolicyLocktime::AbsoluteRenewable] {
        let mut zero = draft(kind);
        zero.count = 0;
        assert_eq!(
            resolve(zero).lock_error,
            PolicyDraftError::Count,
            "zero duration {kind:?}"
        );
    }
    let mut date = draft(PolicyLocktime::Absolute);
    date.date_timestamp = 1_700_000_000;
    assert_eq!(
        resolve(date.clone()).lock_error,
        PolicyDraftError::Future,
        "not in future"
    );
    date.date_valid = false;
    assert_eq!(
        resolve(date).lock_error,
        PolicyDraftError::Date,
        "invalid date"
    );
    for kind in [PolicyLocktime::Absolute, PolicyLocktime::AbsoluteRenewable] {
        let no_tip = resolve_policy_draft(draft(kind), Network::Regtest, false, 0, 1_700_000_000);
        assert_eq!(
            no_tip.lock_error,
            PolicyDraftError::Tip,
            "tip required {kind:?}"
        );
        let overflow = resolve_policy_draft(
            draft(kind),
            Network::Regtest,
            true,
            499_999_999,
            1_700_000_000,
        );
        assert!(!overflow.is_ok, "height boundary {kind:?}");
        assert_eq!(
            overflow.lock_error,
            PolicyDraftError::Bounds,
            "height bound {kind:?}"
        );
    }
    let mut duplicate = draft(PolicyLocktime::None);
    duplicate.keys.push("11111111".into());
    assert!(!resolve(duplicate).is_ok, "duplicate key rejected");
}
