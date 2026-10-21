use std::str::FromStr;

use bitcoin::{bip32::Fingerprint, Network};
use bwk_sp::bwk_tx::bwk_psbt::PsbtV2;
use silent::{
    keys::{
        crypto::Password, Error, KeyManager, KeyManagerImpl, KeyRecord, KeyStore, LiveDevice,
        RouteKind,
    },
    set_datadir, state_crypt,
};
use temp_dir::TempDir;
use zeroize::Zeroizing;

const FIXTURES: [(&str, u32); 3] = [
    (
        include_str!("../../tests/host/fixtures/signer-ordinary.hex"),
        1,
    ),
    (
        include_str!("../../tests/host/fixtures/signer-taproot.hex"),
        2,
    ),
    (
        include_str!("../../tests/host/fixtures/signer-mixed.hex"),
        3,
    ),
];

fn device(id: &str, fingerprint: Fingerprint, capabilities: u32) -> LiveDevice {
    LiveDevice {
        id: id.into(),
        kind: "external".into(),
        model: String::new(),
        serial: id.into(),
        module: "fixture".into(),
        version: String::new(),
        capabilities,
        fingerprint: Some(fingerprint),
    }
}

#[test]
fn actual_psbt_operations_gate_all_sixteen_capability_combinations() {
    let home = TempDir::new().expect("isolated capability state");
    set_datadir(home.path().to_path_buf());
    state_crypt::unlock_for_tests();
    let mut manager = KeyManager::new(
        KeyStore::new(home.path().join("keys"), Network::Regtest),
        Network::Regtest,
    )
    .expect("capability manager");
    let fp = Fingerprint::from_str("aabbccdd").expect("fixture fingerprint");
    manager
        .add_key(KeyRecord::new(fp, Network::Regtest, "fixture".into()))
        .expect("register");
    manager.add_route(fp, RouteKind::Usb).expect("USB route");
    for mask in 0..16 {
        manager.set_live_devices(vec![
            device("unsupported", fp, 0),
            device("capable", fp, mask),
        ]);
        for (fixture, required) in FIXTURES {
            let bytes = hex::decode(fixture.trim()).expect("PSBT hex");
            let plan = manager.signing_plan(&bytes).expect("real PSBT plan");
            assert_eq!(plan.candidates.len(), 1, "one fingerprint, mask {mask}");
            let candidate = &plan.candidates[0];
            let supported = mask & required == required;
            assert_eq!(
                candidate.unusable.is_none(),
                supported,
                "mask {mask}, operations {required}"
            );
            assert_eq!(
                candidate.route,
                supported.then_some(RouteKind::Usb),
                "mask {mask}, operations {required}"
            );
            if supported {
                assert_eq!(
                    candidate.device_id, "capable",
                    "skip the incapable first device"
                );
            }
            let checked = manager.check_device_signing(&bytes, fp, mask);
            assert_eq!(
                checked.is_ok(),
                supported,
                "dispatch check, mask {mask}, operations {required}"
            );
            if !supported {
                assert!(
                    matches!(checked, Err(Error::UnsupportedOperation { required: actual, .. }) if actual == required),
                    "explicit unsupported operations: {checked:?}"
                );
            }
        }
    }
    let other = Fingerprint::from_str("11223344").expect("second fingerprint");
    manager
        .add_key(KeyRecord::new(
            other,
            Network::Regtest,
            "Taproot fixture".into(),
        ))
        .expect("register independent key");
    manager
        .add_route(other, RouteKind::Usb)
        .expect("independent USB route");
    manager.set_live_devices(vec![device("ordinary", fp, 1), device("taproot", other, 2)]);
    let mut mixed = PsbtV2::deserialize(&hex::decode(FIXTURES[2].0.trim()).expect("mixed hex"))
        .expect("mixed PSBT");
    for (_, (origin, _)) in mixed.inputs[1].psbt.tap_key_origins.values_mut() {
        *origin = other;
    }
    let plan = manager
        .signing_plan(&mixed.serialize().expect("independent inputs"))
        .expect("per-fingerprint operations");
    assert_eq!(
        plan.candidates.len(),
        2,
        "both independent keys are required"
    );
    for candidate in &plan.candidates {
        assert!(
            candidate.unusable.is_none(),
            "only this fingerprint's operations are required"
        );
        assert_eq!(
            candidate.device_id,
            if candidate.fingerprint == fp {
                "ordinary"
            } else {
                "taproot"
            },
            "select the device for this input, not the whole transaction's operation mix"
        );
    }
    manager.set_live_devices(vec![device("ordinary", fp, 1), device("taproot", fp, 2)]);
    let plan = manager
        .signing_plan(&hex::decode(FIXTURES[2].0.trim()).expect("mixed hex"))
        .expect("one key, two devices");
    assert!(
        plan.candidates[0].unusable.is_some(),
        "capabilities from different devices cannot be combined into one usable route"
    );
    manager
        .add_route(fp, RouteKind::Qr)
        .expect("fallback route");
    manager.set_live_devices(vec![device("unsupported", fp, 0)]);
    for (fixture, _) in FIXTURES {
        let bytes = hex::decode(fixture.trim()).expect("PSBT hex");
        let plan = manager.signing_plan(&bytes).expect("fallback plan");
        assert_eq!(
            plan.candidates[0].route,
            Some(RouteKind::Qr),
            "skip unsupported USB for QR"
        );
        assert!(
            plan.candidates[0].unusable.is_none(),
            "QR fallback remains usable"
        );
    }
    let hot_fp = manager.import_hot_key(
        Zeroizing::new("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about".into()),
        "Hot fixture".into(), Password::new("fixture".into()),
    ).expect("real Hot key");
    manager.set_live_devices(vec![device("unsupported", hot_fp, 0)]);
    for (fixture, _) in FIXTURES {
        let mut psbt = PsbtV2::deserialize(&hex::decode(fixture.trim()).expect("PSBT hex"))
            .expect("fixture PSBT");
        for input in &mut psbt.inputs {
            for (origin, _) in input.psbt.bip32_derivation.values_mut() {
                *origin = hot_fp;
            }
            for (_, (origin, _)) in input.psbt.tap_key_origins.values_mut() {
                *origin = hot_fp;
            }
        }
        let plan = manager
            .signing_plan(&psbt.serialize().expect("Hot request"))
            .expect("Hot plan");
        assert_eq!(
            plan.candidates[0].route,
            Some(RouteKind::Hot),
            "Hot is not gated by HWI metadata"
        );
        assert!(plan.candidates[0].unusable.is_none(), "Hot remains usable");
    }
}
