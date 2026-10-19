mod common;

use common::{descriptor_from_mnemonic, TEST_MNEMONIC};
use silent::{delete_config, list_configs, set_datadir, Config, Network};
use temp_dir::TempDir;

#[test]
fn test_list_accounts_after_create_delete() {
    let tmp = TempDir::new().expect("create temp dir");
    set_datadir(tmp.path().to_path_buf());
    silent::state_crypt::unlock_for_tests();

    // Initially empty
    assert!(list_configs().is_empty(), "should start empty");

    // Create account "alice"
    let alice_config = Config::new(
        "alice".to_string(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    );
    alice_config.to_file();
    assert_eq!(list_configs(), vec!["alice"]);

    // Create account "bob"
    let bob_config = Config::new(
        "bob".to_string(),
        Network::Regtest,
        descriptor_from_mnemonic(TEST_MNEMONIC, Network::Regtest),
        "http://localhost:50001".to_string(),
        String::new(),
        Some(546),
    );
    bob_config.to_file();

    let mut listed = list_configs();
    listed.sort();
    assert_eq!(listed, vec!["alice", "bob"]);

    // Delete "alice"
    assert!(delete_config("alice".to_string()), "delete should succeed");

    assert_eq!(list_configs(), vec!["bob"]);

    // Delete "bob"
    assert!(delete_config("bob".to_string()), "delete should succeed");

    assert!(
        list_configs().is_empty(),
        "should be empty after deleting all"
    );

    // Deleting non-existent account returns false
    assert!(
        !delete_config("charlie".to_string()),
        "delete non-existent should fail"
    );
}
