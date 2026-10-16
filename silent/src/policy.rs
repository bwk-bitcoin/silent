//! CXX surface over bwk-bup: compile a creator policy, lift a descriptor for review.
//!
//! The policy compiler is not wired yet: every call answers `is_ok = false`.

#![allow(dead_code, unused_variables)]

use crate::ffi::{Network, PolicyCompileResult, PolicyReview, PolicySpec};

const NOT_AVAILABLE: &str = "policy compiler not available yet";

fn not_available() -> PolicyCompileResult {
    PolicyCompileResult {
        is_ok: false,
        error: NOT_AVAILABLE.into(),
        error_path: -1,
        too_many_leaves: false,
        descriptor: String::new(),
        leaf_count: 0,
        max_leaves: 0,
        has_key_path: false,
    }
}

pub fn bip87_account_path(network: Network, account: u32) -> String {
    String::new()
}

pub fn preview_policy(spec: PolicySpec) -> PolicyCompileResult {
    not_available()
}

pub fn compile_policy(spec: PolicySpec) -> PolicyCompileResult {
    not_available()
}

pub fn review_policy(descriptor: String) -> PolicyReview {
    PolicyReview {
        is_ok: false,
        error: NOT_AVAILABLE.into(),
        paths: Vec::new(),
        leaf_count: 0,
    }
}
