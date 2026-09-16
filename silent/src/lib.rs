//! Silent - Bitcoin Silent Payment wallet backed by BlindBit
//!
//! This library provides C++ bindings for the bwk-sp Silent Payment implementation.

pub mod account;
pub mod app_state;
pub mod config;
pub mod hwi;
pub mod hwi_config;
pub mod keys;
pub mod sync_estimator;

use bwk_sp::bwk::bwk_electrum::url::ElectrumScheme;

#[cxx::bridge]
mod ffi {
    // ===== Shared Enums =====

    /// Bitcoin network types.
    #[derive(Debug, Clone, Copy)]
    pub enum Network {
        Regtest,
        Signet,
        Testnet,
        Bitcoin,
    }

    /// Logging levels.
    #[derive(Debug, Clone, Copy)]
    pub enum LogLevel {
        Off,
        Error,
        Warn,
        Info,
        Debug,
        Trace,
    }

    /// Notification types from the scanner.
    #[derive(Debug, Clone, Copy)]
    pub enum NotificationFlag {
        StartingScan,
        ScanStarted,
        FailStartScanning,
        FailScan,
        StoppingScan,
        ScanStopped,
        ScanProgress,
        ScanCompleted,
        NewOutput,
        OutputSpent,
        WaitingForBlocks,
        NewBlocksDetected,
        CoinUpdate,
        PaymentUpdated,
        HeaderProgress,
        AddressTipChanged,
        ElectrumStarted,
        ElectrumConnected,
        ElectrumError,
        ElectrumStopped,
    }

    /// State of a signing device the HWI service can see.
    #[derive(Debug, Clone, Copy)]
    pub enum HwiDeviceState {
        Supported,
        Locked,
        Unsupported,
    }

    /// Kind of notification coming from the HWI service.
    #[derive(Debug, Clone, Copy)]
    pub enum HwiEventFlag {
        DevicesChanged,
        Xpub,
        Version,
        WalletRegistered,
        WalletIsRegistered,
        TransactionSigned,
        AddressDisplayed,
        Error,
    }

    /// How a key can be reached, in priority order. Hot always outranks the others.
    /// `Unknown` never appears in `add_route`/`remove_route`/`set_route_order` calls;
    /// it only shows up in a `SigningCandidate.route` for a fingerprint with no
    /// configured route, where `can_sign` is false.
    #[derive(Debug, Clone, Copy)]
    pub enum KeyRouteKind {
        Hot,
        Usb,
        Qr,
        SdCard,
        Unknown,
    }

    /// Why a key cannot sign the PSBT in hand. `None` means it can.
    #[derive(Debug, Clone, Copy)]
    pub enum KeyUnusableReason {
        None,
        NotRegistered,
        NoRoute,
        DeviceNotConnected,
        NoHotSecret,
        SpOutputsPending,
    }

    /// Kind of a decoded and validated QR response.
    #[derive(Debug, Clone, Copy)]
    pub enum QrResponseKind {
        Xpubs,
        Registration,
        AddressUri,
        SignedPsbt,
        SpDescriptor,
    }

    /// Kind of notification coming from the keys module. `ScanProgress` is a
    /// push-only signal to go read the current values from the polled
    /// `camera_progress()` accessor; it carries no progress data itself.
    #[derive(Debug, Clone, Copy)]
    pub enum KeyEventFlag {
        QrDecoded,
        ScanProgress,
        CameraStopped,
        Error,
    }

    // ===== Shared Structs =====

    /// Notification from the account scanner.
    #[derive(Debug, Clone)]
    pub struct Notification {
        pub flag: NotificationFlag,
        pub payload: String,
    }

    /// A coin entry from the wallet.
    #[derive(Debug, Clone)]
    pub struct RustCoin {
        pub outpoint: String,
        pub value: u64,
        pub height: u32,
        pub label: String,
        pub spent: bool,
        pub account_type: String,
    }

    /// Summary of spendable coins.
    #[derive(Debug, Clone)]
    pub struct CoinState {
        pub confirmed_count: u64,
        pub confirmed_balance: u64,
        pub unconfirmed_count: u64,
        pub unconfirmed_balance: u64,
    }

    /// A transaction entry from payment history.
    #[derive(Debug, Clone)]
    pub struct RustTx {
        pub txid: String,
        pub direction: String,
        pub amount: u64,
        pub fee: u64,
        pub height: u32,
    }

    /// Transaction output specification.
    #[derive(Debug, Clone)]
    pub struct Output {
        /// Recipient address (SP address, legacy address, or hex data for OP_RETURN)
        pub address: String,
        /// Amount in satoshis (must be 0 for OP_RETURN outputs)
        pub amount: u64,
        /// Optional label for this output
        pub label: String,
        /// If true, send maximum possible amount (minus fees) to this output
        pub max: bool,
    }

    /// Template for creating a transaction.
    #[derive(Debug, Clone)]
    pub struct TransactionTemplate {
        /// List of output specifications
        pub outputs: Vec<Output>,
        /// Fee rate in sat/vbyte (used when fee == 0)
        pub fee_rate: f64,
        /// Absolute fee in satoshis (takes precedence over fee_rate when > 0)
        pub fee: u64,
        /// Optional: specific UTXOs to use (empty = automatic selection)
        pub input_outpoints: Vec<String>,
    }

    /// Result of transaction simulation.
    #[derive(Debug, Clone)]
    pub struct TransactionSimulation {
        /// Whether the transaction can be created with current funds
        pub is_valid: bool,
        /// Estimated fee in satoshis (0 if invalid)
        pub fee: u64,
        /// Estimated weight in weight units (0 if invalid)
        pub weight: u64,
        /// Total input amount in satoshis (0 if invalid)
        pub input_total: u64,
        /// Total output amount in satoshis (0 if invalid)
        pub output_total: u64,
        /// Number of inputs selected (0 if invalid)
        pub input_count: u64,
        /// Error message (empty if valid)
        pub error: String,
        /// Outpoints selected by coin selection (empty if invalid)
        pub selected_outpoints: Vec<String>,
    }

    /// Result of a transaction operation (sign, broadcast).
    #[derive(Debug, Clone)]
    pub struct TxResult {
        /// Whether the operation succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// Result value: signed tx hex or txid (empty on error).
        pub value: String,
    }

    /// Result of decoding an SP descriptor QR response.
    #[derive(Debug, Clone)]
    pub struct SpDescriptorResult {
        /// Whether the decode succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// Decoded SP descriptor (empty on error).
        pub descriptor: String,
        /// Origin fingerprint of the sp() descriptor (empty on error).
        pub fingerprint: String,
    }

    /// Grayscale QR image for the C++ UI.
    #[derive(Debug, Clone)]
    pub struct QrImageResult {
        /// Whether rendering succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// One byte per pixel: 0 is black, 255 is white.
        pub data: Vec<u8>,
        pub width: u32,
        pub height: u32,
    }

    /// Result of a connection test.
    #[derive(Debug, Clone)]
    pub struct ConnectionResult {
        /// Whether the connection succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// Normalized URL that successfully connected (with scheme).
        pub url: String,
    }

    /// Default regtest infrastructure addresses fetched from minta API.
    #[derive(Debug, Clone)]
    pub struct RegtestDefaults {
        /// Whether the fetch succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// BlindBit server URL (with http:// prefix).
        pub blindbit_url: String,
        /// Electrum server address (host:port).
        pub electrum_url: String,
    }

    /// Result of PSBT pre-signing validation via Electrum.
    #[derive(Debug, Clone)]
    pub struct PsbtValidation {
        /// Whether the validation check itself succeeded (false = connection/protocol error).
        pub is_ok: bool,
        /// Error message when is_ok is false (connection failure, etc.).
        pub error: String,
        /// Whether the PSBT passed all checks (true = no issues found).
        pub is_valid: bool,
        /// Human-readable summary of issues found.
        pub issues: String,
        /// Number of output addresses that have been used before.
        pub reused_output_count: u32,
        /// Number of inputs whose coins have already been spent.
        pub spent_input_count: u32,
    }

    /// Backend server information result.
    #[derive(Debug, Clone)]
    pub struct BackendInfo {
        /// Whether the request succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// Normalized URL that successfully connected (with scheme).
        pub url: String,
        /// Network reported by the backend.
        pub network: Network,
        /// Current block height.
        pub height: u32,
        /// Backend supports tweaks-only mode.
        pub tweaks_only: bool,
        /// Backend supports full basic tweaks.
        pub tweaks_full_basic: bool,
        /// Backend supports full tweaks with dust filter.
        pub tweaks_full_with_dust_filter: bool,
        /// Backend supports cut-through with dust filter.
        pub tweaks_cut_through_with_dust_filter: bool,
    }

    /// One signing device as the service currently sees it.
    #[derive(Debug, Clone)]
    pub struct HwiDevice {
        /// Stable, serial-based where the device provides one.
        pub id: String,
        pub kind: String,
        /// Empty unless the device is Supported.
        pub fingerprint: String,
        /// Empty when the device did not report one.
        pub version: String,
        pub state: HwiDeviceState,
        /// Pairing code when Locked, the reason when Unsupported, else empty.
        pub detail: String,
    }

    /// A notification from the HWI service. `req_id` is the id the caller passed to the
    /// request it answers, and 0 for an unsolicited push.
    #[derive(Debug, Clone)]
    pub struct HwiEvent {
        pub flag: HwiEventFlag,
        pub req_id: u64,
        pub fingerprint: String,
        pub payload: String,
        pub error: String,
        /// WalletIsRegistered answers with it; other kinds set it on success.
        pub ok: bool,
    }

    /// A registered key as the Keys tab shows it.
    #[derive(Debug, Clone)]
    pub struct KeyInfo {
        pub fingerprint: String,
        pub alias: String,
        pub has_hot_secret: bool,
        /// In priority order, first entry is the preferred route.
        pub routes: Vec<KeyRouteKind>,
        pub is_connected: bool,
        pub device_count: u32,
        pub descriptor_count: u32,
    }

    /// A device remembered for a key. `model` and `serial` are empty until bwk-hwi
    /// reports them; they exist only to deduplicate devices sharing a fingerprint.
    /// `device_id` and `brand` are always empty: `DeviceRecord` carries neither a
    /// stable id nor a brand today.
    #[derive(Debug, Clone)]
    pub struct KeyDevice {
        pub fingerprint: String,
        pub device_id: String,
        pub kind: String,
        pub brand: String,
        pub model: String,
        pub serial: String,
        pub module: String,
    }

    /// A device the signer bus can see right now, fed in from the hwi module.
    /// `brand` is always empty: `manager::LiveDevice` carries no brand field
    /// because bwk-hwi has not reported one yet.
    #[derive(Debug, Clone)]
    pub struct LiveDevice {
        pub device_id: String,
        pub fingerprint: String,
        pub kind: String,
        pub brand: String,
        pub model: String,
        pub serial: String,
        pub module: String,
    }

    /// A cached extended public key at a derivation path for a registered key.
    #[derive(Debug, Clone)]
    pub struct KeyXpub {
        pub fingerprint: String,
        pub path: String,
        pub xpub: String,
    }

    /// A descriptor a key participates in.
    #[derive(Debug, Clone)]
    pub struct KeyDescriptor {
        pub fingerprint: String,
        pub descriptor: String,
        /// The sha256-prefix alias the device knows this descriptor by.
        pub alias: String,
        pub registered: bool,
    }

    /// Result of a keys module operation carrying no response data.
    #[derive(Debug, Clone)]
    pub struct KeyOpResult {
        /// Whether the operation succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
    }

    /// Result of `qr_reserve_sign`. `id` is meaningless when `is_ok` is false: unlike
    /// the plain `u64` this replaces, failure never overlaps a real reserved id (`0`
    /// is reserved for "no outstanding request" and is never issued by `reserve_id`).
    #[derive(Debug, Clone)]
    pub struct QrReserveResult {
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        pub id: u64,
    }

    /// Result of a keys module operation carrying a `KeyInfo`.
    #[derive(Debug, Clone)]
    pub struct KeyInfoResult {
        /// Whether the operation succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// The key info (empty/zeroed fields on error).
        pub key: KeyInfo,
    }

    /// One row of the signer picker. The picker is PSBT driven: every fingerprint the
    /// PSBT needs gets an entry. Connected devices unrelated to the PSBT are not listed
    /// here; the C++ picker derives those hints from its live signers.
    #[derive(Debug, Clone)]
    pub struct SigningCandidate {
        pub fingerprint: String,
        pub alias: String,
        /// Meaningless when `can_sign` is false; `Unknown` is the sentinel value.
        pub route: KeyRouteKind,
        /// Set only when `route` is Usb: the id C++ passes to the device's own ISigner.
        pub device_id: String,
        pub can_sign: bool,
        /// Why not, when `can_sign` is false. `None` otherwise.
        pub unusable: KeyUnusableReason,
        /// The SP key, which completes the output scripts before anyone else signs.
        pub must_sign_first: bool,
        pub registered: bool,
    }

    /// Signer picker for one PSBT, built by `KeysModule::signing_plan`.
    #[derive(Debug, Clone)]
    pub struct SigningPlan {
        /// Whether the plan could be built at all (false on a malformed PSBT, for
        /// example). Unrelated to whether any candidate `can_sign`.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// Ordered: `must_sign_first` first, then usable, then unusable.
        pub candidates: Vec<SigningCandidate>,
        /// An output script is still underived, so only the SP key may sign now.
        pub sp_outputs_pending: bool,
    }

    /// Result of a fingerprint lookup carrying no key/module state.
    #[derive(Debug, Clone)]
    pub struct FingerprintsResult {
        /// Whether the lookup succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        pub fingerprints: Vec<String>,
    }

    /// Result of a mnemonic operation (generation or reveal).
    #[derive(Debug, Clone)]
    pub struct MnemonicResult {
        /// Whether the operation succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// Empty on error. Never holds a partial or placeholder phrase.
        pub mnemonic: String,
    }

    /// Result of a hot signing operation.
    #[derive(Debug, Clone)]
    pub struct SignResult {
        /// Whether the signing operation succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// The signed PSBT, serialized v2 and hex encoded. Empty on error.
        pub psbt_v2_hex: String,
    }

    /// Result of converting a PSBT v2 into its v0 base64 payload for a USB signing
    /// device.
    #[derive(Debug, Clone)]
    pub struct PsbtPayloadResult {
        /// Whether the conversion succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// The v0 PSBT, base64 encoded. Empty on error.
        pub psbt_base64: String,
    }

    /// A notification from the keys module. `req_id` is the QR request id the event
    /// answers, and 0 for an unsolicited push.
    #[derive(Debug, Clone)]
    pub struct KeyEvent {
        pub flag: KeyEventFlag,
        pub req_id: u64,
        pub fingerprint: String,
        pub payload: String,
        pub error: String,
        pub ok: bool,
    }

    #[derive(Debug, Clone)]
    pub struct CameraListResult {
        pub is_ok: bool,
        pub error: String,
        pub names: Vec<String>,
        /// Parallel to `names`: the index to pass to camera_start.
        pub indexes: Vec<u32>,
    }

    /// Three bytes per pixel, interleaved RGB888.
    #[derive(Debug, Clone)]
    pub struct CameraFrameResult {
        pub is_ok: bool,
        pub error: String,
        pub data: Vec<u8>,
        pub width: u32,
        pub height: u32,
        /// Monotonic. C++ repaints only when it changes.
        pub seq: u64,
    }

    #[derive(Debug, Clone)]
    pub struct ScanProgress {
        pub is_ok: bool,
        pub error: String,
        pub seen: u32,
        pub total: u32,
        pub seq: u64,
    }

    #[derive(Debug, Clone)]
    pub struct PsbtFileResult {
        pub is_ok: bool,
        pub error: String,
        /// The PSBT read from disk, hex encoded. Empty on save and on error.
        pub psbt_hex: String,
    }

    // ===== Opaque Rust Types =====

    extern "Rust" {
        /// Configuration for a Silent account.
        type Config;

        /// Account instance.
        type Account;

        /// Poll result for checking notifications.
        type Poll;

        /// Result wrapper for unsigned transactions.
        type PsbtResult;

        /// Notification receiver for blocking recv in a dedicated thread.
        type NotificationReceiver;

        /// EMA-based sync time estimator.
        type SyncEstimator;

        /// Hardware wallet device service.
        type HwiService;

        /// Notification receiver for the HWI service.
        type HwiReceiver;

        /// Poll result for HWI notifications.
        type HwiPoll;

        /// Owns the key manager for one network: records, routes, devices, the xpub
        /// cache and registered descriptors.
        type KeysModule;

        /// One encoded QR request the C++ modal pulls animation frames from.
        type QrRequest;

        /// A decoded and validated QR response.
        type QrResponse;

        /// Notification receiver for the keys module's camera/QR event channel.
        type KeysReceiver;

        /// Poll result for keys module notifications.
        type KeysPoll;

        /// GUI-thread copy of one key's record and the manager's network, for the
        /// free hot-key worker functions to sign/reveal/derive from off-thread.
        type HotMaterial;

        /// A mnemonic sealed by `hot_seal`, committed by `KeysModule::commit_hot_key`.
        type SealedHotKey;

        /// A camera opened by `camera_open`, installed by `KeysModule::camera_install`.
        type CameraHandle;
    }

    // ===== Utility Functions =====

    extern "Rust" {
        /// Initialize logging with the specified level.
        /// Call this once at application startup.
        fn init_logging(level: LogLevel);

        fn notification_to_string(notif: &Notification) -> String;

        /// Query backend server info (network, height, capabilities).
        /// Blocking HTTP call - may take up to 30s on timeout.
        fn get_backend_info(blindbit_url: String) -> BackendInfo;

        /// Validate a BIP392 Silent Payments descriptor.
        fn validate_sp_descriptor(descriptor: String) -> bool;

        /// Encode a GetSpDescriptor QR request as base64.
        fn qr_get_sp_descriptor_request() -> String;

        /// Render a GetSpDescriptor QR request.
        fn qr_get_sp_descriptor_request_image() -> QrImageResult;

        /// Decode a base64 SpDescriptor QR response.
        fn qr_decode_sp_descriptor_response(payload: String) -> SpDescriptorResult;

        /// Parses an sp() descriptor and requires an origin. Returns the descriptor
        /// unchanged plus its fingerprint, or an error saying the origin is missing.
        fn validate_sp_descriptor_origin(descriptor: String) -> SpDescriptorResult;

        /// Every fingerprint the PSBT needs a signature from, lowercase hex,
        /// deduplicated and sorted. Walks bip32_derivation, tap_key_origins and the
        /// silent payment spend derivation.
        fn psbt_required_fingerprints(psbt_v2_hex: &str) -> FingerprintsResult;

        /// Validate a recipient address (SP address, legacy Bitcoin address, or hex data).
        /// Returns empty string if valid, or error message if invalid.
        fn validate_address(address: String) -> String;

        /// Test Electrum server connectivity.
        /// Blocking call - attempts TCP connect and server.version handshake.
        fn test_electrum(address: String) -> ConnectionResult;

        /// Fetch default regtest infrastructure addresses from minta API.
        /// Blocking HTTP call - should be called from a background thread.
        fn get_regtest_defaults() -> RegtestDefaults;

        /// List enabled app-level plugin IDs.
        fn app_enabled_plugins() -> Vec<String>;

        /// Enable or disable a plugin ID at app level.
        fn app_set_plugin_enabled(id: String, enabled: bool);

        /// Get the active app theme name.
        fn app_active_theme() -> String;

        /// Set the active app theme name.
        fn app_set_active_theme(name: String);

        /// Writes the PSBT to `path` exactly as serialized. Refuses to overwrite.
        fn sd_save_psbt(path: &str, psbt_hex: &str) -> PsbtFileResult;
        fn sd_load_psbt(path: &str) -> PsbtFileResult;
    }

    // ===== Config Methods =====

    extern "Rust" {
        /// Create a new config.
        fn new_config(
            account_name: String,
            network: Network,
            descriptor: String,
            blindbit_url: String,
            electrum_url: String,
            dust_limit: u64,
            plugin_id: String,
        ) -> Box<Config>;

        /// Load config from file.
        fn config_from_file(account_name: String) -> Box<Config>;

        /// List all existing configs.
        fn list_configs() -> Vec<String>;

        /// Delete an account's data from disk.
        fn delete_config(account_name: String) -> bool;

        /// Check if the config was loaded successfully.
        fn is_ok(self: &Config) -> bool;

        /// Get error message (empty if is_ok).
        fn get_error(self: &Config) -> String;

        /// Save config to file.
        fn to_file(self: &Config);

        /// Get SP descriptor.
        fn get_descriptor(self: &Config) -> String;

        /// Set SP descriptor.
        fn set_descriptor(self: &mut Config, descriptor: String);

        /// Get network.
        fn get_network(self: &Config) -> Network;

        /// Set network.
        fn set_network(self: &mut Config, network: Network);

        /// Get BlindBit URL.
        fn get_blindbit_url(self: &Config) -> String;

        /// Set BlindBit URL.
        fn set_blindbit_url(self: &mut Config, url: String);

        /// Get Electrum URL.
        fn get_electrum_url(self: &Config) -> String;

        /// Set Electrum URL.
        fn set_electrum_url(self: &mut Config, url: String);

        /// Get dust limit (0 means not set).
        fn get_dust_limit(self: &Config) -> u64;

        /// Set dust limit (0 to unset).
        fn set_dust_limit(self: &mut Config, limit: u64);

        /// Get plugin/module identifier.
        fn get_plugin_id(self: &Config) -> String;

        /// Set plugin/module identifier.
        fn set_plugin_id(self: &mut Config, plugin_id: String);
    }

    // ===== Account Methods =====

    extern "Rust" {
        /// Create a new account from an account name (loads config from file).
        /// Check is_ok() before use; get_error() for failure reason.
        fn new_account(account_name: String) -> Box<Account>;

        /// Check if the account was created successfully.
        fn is_ok(self: &Account) -> bool;

        /// Get error message (empty if is_ok).
        fn get_error(self: &Account) -> String;

        /// Start the scanner.
        fn start_scanner(self: &mut Account) -> bool;

        /// Stop the scanner.
        fn stop_scanner(self: &mut Account);

        /// Stop all electrum listeners on sub-accounts.
        fn stop_electrum(self: &mut Account);

        /// Reload electrum config and start listeners.
        fn start_electrum(self: &mut Account) -> bool;

        /// Try to receive a notification (non-blocking).
        fn try_recv(self: &mut Account) -> Box<Poll>;

        /// Take ownership of the notification receiver for use in a dedicated thread.
        fn take_receiver(self: &mut Account) -> Box<NotificationReceiver>;

        /// Get account name.
        fn name(self: &Account) -> String;

        /// Get balance in satoshis.
        fn balance(self: &Account) -> u64;

        /// Get the Silent Payment address.
        fn sp_address(self: &Account) -> String;

        /// Check if the account has sub-accounts (segwit + taproot).
        fn has_sub_accounts(self: &Account) -> bool;

        /// Get the number of sub-accounts.
        fn sub_account_count(self: &Account) -> u32;

        /// Generate a new segwit (wpkh) receiving address.
        fn new_segwit_addr(self: &mut Account) -> String;

        /// Generate a new taproot receiving address.
        fn new_taproot_addr(self: &mut Account) -> String;

        /// Get all coins (spent and unspent).
        fn coins(self: &Account) -> Vec<RustCoin>;

        /// Get spendable coins summary.
        fn spendable_coins(self: &Account) -> CoinState;

        /// Update a coin's label.
        fn update_coin_label(self: &mut Account, outpoint: String, label: String) -> bool;

        /// Get payment history.
        fn payment_history(self: &Account) -> Vec<RustTx>;

        /// Get the account SP descriptor.
        fn descriptor(self: &Account) -> String;

        /// Simulate a transaction to check feasibility and estimate fees.
        /// Returns simulation result with fee, weight, and validity info.
        fn simulate_transaction(
            self: &Account,
            tx_template: TransactionTemplate,
        ) -> TransactionSimulation;

        /// Prepare a transaction for signing.
        /// Creates an unsigned transaction from the template and returns a PsbtResult.
        /// The transaction will be finalized and ready to sign.
        fn prepare_transaction(self: &Account, tx_template: TransactionTemplate)
            -> Box<PsbtResult>;

        /// Sign a prepared transaction.
        /// Returns TxResult with signed tx hex in value.
        fn sign_transaction(self: &Account, psbt_result: &PsbtResult) -> TxResult;

        /// Broadcast a signed transaction (hex string) to the network.
        /// Returns TxResult with txid in value.
        fn broadcast_transaction(self: &Account, signed_tx_hex: String) -> TxResult;

        /// Finalize a signed PSBT and return transaction hex.
        fn finalize_psbt(self: &Account, signed_psbt_base64: String) -> TxResult;

        /// Sign and broadcast a transaction in one step.
        /// Returns TxResult with txid in value.
        fn sign_and_broadcast(self: &Account, psbt_result: &PsbtResult) -> TxResult;

        /// Log all details of a failed broadcast for debugging/reproduction.
        /// Called after broadcast_transaction returns an error.
        fn log_failed_broadcast(
            self: &Account,
            tx_template: TransactionTemplate,
            signed_tx_hex: String,
        );

        /// Validate a prepared transaction via Electrum before signing.
        /// Checks for output address reuse and already-spent inputs.
        /// Blocking call - must be called from a background thread.
        fn validate_before_sign(self: &Account, psbt_result: &PsbtResult) -> PsbtValidation;
    }

    // ===== Poll Methods =====

    extern "Rust" {
        /// Check if poll has a notification.
        fn is_some(self: &Poll) -> bool;

        /// Get the notification (only valid if is_some() == true).
        fn get_notification(self: &Poll) -> Notification;

        /// Get error message (if any).
        fn get_error(self: &Poll) -> String;
    }

    // ===== NotificationReceiver Methods =====

    extern "Rust" {
        /// Blocking receive — waits for the next notification.
        /// Returns Poll with is_some()=false when the channel disconnects.
        fn recv(self: &NotificationReceiver) -> Box<Poll>;
    }

    // ===== HWI Service Methods =====

    extern "Rust" {
        /// Create the device service. Does not start polling.
        fn new_hwi_service(network: Network) -> Box<HwiService>;

        /// Start polling for devices. Idempotent.
        fn start(self: &mut HwiService);

        /// Stop polling. Idempotent.
        fn stop(self: &mut HwiService);

        /// Switch networks in place, keeping the notification channel. Does not block
        /// on the old listener, so it is safe to call from the GUI thread.
        fn set_network(self: &mut HwiService, network: Network);

        /// Stop for good and end the notification stream.
        fn shutdown(self: &mut HwiService);

        fn is_running(self: &HwiService) -> bool;

        /// Take the receiver for a dedicated notification thread. Call once.
        fn take_receiver(self: &mut HwiService) -> Box<HwiReceiver>;

        /// Every device the service can see, in whatever state.
        fn devices(self: &HwiService) -> Vec<HwiDevice>;

        /// Requests. Each returns whether it was dispatched; the result arrives on the
        /// notification carrying the same req_id.
        fn get_xpub(self: &HwiService, req_id: u64, device_id: &str, path: &str) -> bool;
        fn is_descriptor_registered(
            self: &HwiService,
            req_id: u64,
            device_id: &str,
            descriptor: &str,
        ) -> bool;
        fn register_descriptor(
            self: &HwiService,
            req_id: u64,
            device_id: &str,
            descriptor: &str,
        ) -> bool;
        fn sign_psbt(self: &HwiService, req_id: u64, device_id: &str, psbt_base64: &str) -> bool;

        /// Asks the device to show the address at `path`. `descriptor` picks the script
        /// kind: a taproot single key path is shown directly, anything else needs the
        /// wallet to be registered first.
        fn display_address(
            self: &HwiService,
            req_id: u64,
            device_id: &str,
            descriptor: &str,
            path: &str,
        ) -> bool;
    }

    // ===== HwiReceiver / HwiPoll Methods =====

    extern "Rust" {
        /// Blocking receive. Returns a poll with is_some()=false once shut down.
        fn recv(self: &HwiReceiver) -> Box<HwiPoll>;

        fn is_some(self: &HwiPoll) -> bool;
        fn get_event(self: &HwiPoll) -> HwiEvent;
    }

    // ===== HWI Config Methods =====

    extern "Rust" {
        /// Network the device service runs on, persisted in the module's config file.
        fn hwi_config_network() -> Network;
        fn hwi_set_config_network(network: Network);
    }

    // Thread contract for every "Keys" section below: only a free function (no
    // `self` receiver) may run off the GUI thread. Every `self: &KeysModule` or
    // `self: &mut KeysModule` method touches shared state and is GUI-thread only.

    // ===== Keys Module Methods =====

    extern "Rust" {
        fn new_keys_module(network: Network) -> Box<KeysModule>;
        fn is_ok(self: &KeysModule) -> bool;
        fn get_error(self: &KeysModule) -> String;
        /// Reloads the records for `network`. The Keys tab combobox drives this.
        /// On failure the previously-loaded manager is left untouched; only the
        /// error returned by `get_error()` changes.
        fn set_network(self: &mut KeysModule, network: Network) -> KeyOpResult;
        /// Returns `Regtest` as a sentinel if the module has no inner manager.
        /// Callers MUST check `is_ok()` before trusting this value.
        fn network(self: &KeysModule) -> Network;

        /// Stop the camera for good and end the notification stream.
        fn shutdown(self: &mut KeysModule);

        /// Take the receiver for a dedicated notification thread. Call once.
        fn take_receiver(self: &mut KeysModule) -> Box<KeysReceiver>;

        fn keys(self: &KeysModule) -> Vec<KeyInfo>;
        fn key(self: &KeysModule, fingerprint: &str) -> KeyInfoResult;
        fn add_key(self: &mut KeysModule, fingerprint: &str, alias: &str) -> KeyInfoResult;
        /// Refuses while descriptors are registered unless `force` is set; the UI sets
        /// it only after warning.
        fn remove_key(self: &mut KeysModule, fingerprint: &str, force: bool) -> KeyOpResult;
        fn set_alias(self: &mut KeysModule, fingerprint: &str, alias: &str) -> KeyOpResult;

        fn add_route(self: &mut KeysModule, fingerprint: &str, kind: KeyRouteKind) -> KeyOpResult;
        fn remove_route(
            self: &mut KeysModule,
            fingerprint: &str,
            kind: KeyRouteKind,
        ) -> KeyOpResult;
        /// `order` is the full route list, highest priority first.
        fn set_route_order(
            self: &mut KeysModule,
            fingerprint: &str,
            order: Vec<KeyRouteKind>,
        ) -> KeyOpResult;

        /// Replaces the live list wholesale; the C++ side calls this on every
        /// devicesChanged from the hwi module.
        fn set_live_devices(self: &mut KeysModule, devices: Vec<LiveDevice>);
        fn live_devices(self: &KeysModule) -> Vec<LiveDevice>;
        /// Remembers a device for the key with that fingerprint.
        fn record_device(self: &mut KeysModule, device: LiveDevice) -> KeyOpResult;
        fn key_devices(self: &KeysModule, fingerprint: &str) -> Vec<KeyDevice>;

        fn cache_xpub(
            self: &mut KeysModule,
            fingerprint: &str,
            path: &str,
            xpub: &str,
        ) -> KeyOpResult;
        fn key_xpubs(self: &KeysModule, fingerprint: &str) -> Vec<KeyXpub>;
        fn register_descriptor(
            self: &mut KeysModule,
            fingerprint: &str,
            descriptor: &str,
        ) -> KeyOpResult;
        fn unregister_descriptor(
            self: &mut KeysModule,
            fingerprint: &str,
            descriptor: &str,
        ) -> KeyOpResult;
        fn key_descriptors(self: &KeysModule, fingerprint: &str) -> Vec<KeyDescriptor>;
        /// Every registered key whose descriptor list contains this descriptor.
        fn keys_for_descriptor(self: &KeysModule, descriptor: &str) -> Vec<KeyInfo>;

        /// Builds the signer picker for this PSBT. Blocks on nothing; safe to call
        /// from the GUI thread.
        fn signing_plan(self: &KeysModule, psbt_v2_hex: &str) -> SigningPlan;

        /// 12 or 24 words. Any other count is an error. Fast: only draws entropy and
        /// encodes a mnemonic, no password involved, so unlike the other hot
        /// functions below it does not block on Argon2id and is safe to call from
        /// the GUI thread.
        fn generate_mnemonic(word_count: u32) -> MnemonicResult;

        /// Copies out the record and the manager's network for the free hot-key
        /// worker functions below, so a `set_network` or route change afterward
        /// cannot race a worker already in flight against the copy. Fails for an
        /// unknown key or a key without a secret.
        fn hot_material(self: &KeysModule, fingerprint: &str) -> Box<HotMaterial>;
        fn is_ok(self: &HotMaterial) -> bool;
        fn get_error(self: &HotMaterial) -> String;

        /// Derives the fingerprint and seals `mnemonic` under `password` for
        /// `network`, touching no `KeysModule` state. Blocks on Argon2id: call from
        /// a QThread, never from the GUI thread.
        fn hot_seal(mnemonic: &str, password: &str, network: Network) -> Box<SealedHotKey>;
        fn is_ok(self: &SealedHotKey) -> bool;
        fn get_error(self: &SealedHotKey) -> String;
        /// Empty on error.
        fn fingerprint(self: &SealedHotKey) -> String;

        /// Commits a key `hot_seal` built into the store, adding the Hot route in
        /// the same update. Refuses a sealed key whose network does not match the
        /// manager's.
        fn commit_hot_key(
            self: &mut KeysModule,
            sealed: &SealedHotKey,
            alias: &str,
        ) -> KeyInfoResult;

        /// Blocks on Argon2id while opening the envelope: call from a QThread,
        /// never from the GUI thread.
        fn hot_reveal(material: &HotMaterial, password: &str) -> MnemonicResult;

        /// Signs `psbt_v2_hex` with `material`'s hot secret and returns the signed
        /// PSBT as v2 hex. Blocks on Argon2id while opening the envelope: call from
        /// a QThread, never from the GUI thread.
        fn hot_sign(material: &HotMaterial, password: &str, psbt_v2_hex: &str) -> SignResult;

        /// The BIP392 sp() descriptor of `material`'s hot key, origin included, for
        /// account creation. Blocks on Argon2id while opening the envelope: call
        /// from a QThread, never from the GUI thread.
        fn hot_sp_descriptor(material: &HotMaterial, password: &str) -> SpDescriptorResult;

        /// Converts a v2 PSBT into its v0 base64 payload for a USB signing device.
        /// Cheap; safe to call from the GUI thread. Refuses a PSBT with a pending SP
        /// output: a signing device only ever signs its own bip32 inputs, and the
        /// signing plan already blocks every other candidate while one is pending.
        fn usb_sign_payload(psbt_v2_hex: &str) -> PsbtPayloadResult;

        /// Merges a USB device's v0 signed answer back into the original v2 PSBT,
        /// keeping its silent-payment input tweaks and ECDH shares. Only signature
        /// fields are copied. Cheap; safe to call from the GUI thread.
        fn usb_merge_signed(original_v2_hex: &str, signed_v0_base64: &str) -> SignResult;
    }

    // ===== Keys QR Methods =====

    extern "Rust" {
        fn qr_request_get_xpubs(
            self: &mut KeysModule,
            paths: Vec<String>,
            part_bytes: u32,
        ) -> Box<QrRequest>;
        fn qr_request_register_descriptor(
            self: &mut KeysModule,
            descriptor: &str,
            part_bytes: u32,
        ) -> Box<QrRequest>;
        fn qr_request_verify_address(
            self: &mut KeysModule,
            descriptor: &str,
            path: &str,
            address: &str,
            part_bytes: u32,
        ) -> Box<QrRequest>;
        /// Reserves the id a signing QR request will be built under. The build itself
        /// runs on a background thread through `qr_build_sign`, committed back with
        /// `qr_commit_request` once done.
        fn qr_reserve_sign(self: &mut KeysModule) -> QrReserveResult;
        /// Builds the signing QR request `qr_reserve_sign` reserved. Touches no
        /// `KeysModule` state: the one QR builder meant to run off the GUI thread,
        /// inside a `QThread::create` lambda.
        fn qr_build_sign(
            req_id: u64,
            descriptors: Vec<String>,
            psbt_v2_hex: &str,
            part_bytes: u32,
        ) -> Box<QrRequest>;
        /// Commits a request `qr_build_sign` built back into the session, on the GUI
        /// thread. Errors if a newer id has been reserved since (a superseded density
        /// rebuild) instead of silently dropping it.
        fn qr_commit_request(self: &mut KeysModule, request: &QrRequest) -> KeyOpResult;
        fn qr_request_get_sp_descriptor(self: &mut KeysModule, part_bytes: u32) -> Box<QrRequest>;

        fn is_ok(self: &QrRequest) -> bool;
        fn get_error(self: &QrRequest) -> String;
        fn req_id(self: &QrRequest) -> u64;
        fn frame_count(self: &QrRequest) -> u32;
        /// Frame `index` of the animated request. An index past the end answers with
        /// is_ok false, never with frame 0.
        fn frame(self: &QrRequest, index: u32) -> QrImageResult;
        /// The whole encoded request, base64, for the paste box.
        fn base64(self: &QrRequest) -> String;

        /// Validates a pasted payload against the outstanding request id.
        fn qr_decode_response(self: &mut KeysModule, payload: &str) -> Box<QrResponse>;

        fn is_ok(self: &QrResponse) -> bool;
        fn get_error(self: &QrResponse) -> String;
        /// Only meaningful when is_ok().
        fn kind(self: &QrResponse) -> QrResponseKind;
        /// The signer's fingerprint when kind() is Xpubs or SpDescriptor, else empty.
        fn fingerprint(self: &QrResponse) -> String;
        /// The SP descriptor when kind() is SpDescriptor, else empty.
        fn descriptor(self: &QrResponse) -> String;
        /// The signed PSBT as v2 hex when kind() is SignedPsbt, else empty.
        fn psbt_hex(self: &QrResponse) -> String;
        /// The derived xpubs when kind() is Xpubs, else empty.
        fn xpubs(self: &QrResponse) -> Vec<KeyXpub>;
        /// The address URI when kind() is AddressUri, else empty.
        fn uri(self: &QrResponse) -> String;
        fn registered(self: &QrResponse) -> bool;
    }

    // ===== Keys Camera Methods =====

    extern "Rust" {
        fn camera_list(self: &KeysModule) -> CameraListResult;
        /// Opens camera `index` on a fresh channel, touching no `KeysModule` state.
        /// Only blocks briefly to spawn the capture thread: the device itself opens
        /// asynchronously inside `capture_loop`, with success or failure reported
        /// later over the channel as a `CameraMessage`. Still call from a QThread,
        /// never from the GUI thread, for consistency with the other worker
        /// functions.
        fn camera_open(index: u32) -> Box<CameraHandle>;
        fn is_ok(self: &CameraHandle) -> bool;
        fn get_error(self: &CameraHandle) -> String;
        /// Stops the current camera, if any, and stores the one `camera_open`
        /// built.
        fn camera_install(self: &mut KeysModule, handle: Box<CameraHandle>) -> KeyOpResult;
        /// Idempotent.
        fn camera_stop(self: &mut KeysModule);
        fn camera_frame(self: &KeysModule) -> CameraFrameResult;
        fn camera_progress(self: &KeysModule) -> ScanProgress;
        /// Drops the partial BBQR reassembly, for a rescan.
        fn camera_reset(self: &mut KeysModule);
    }

    // ===== KeysReceiver / KeysPoll Methods =====

    extern "Rust" {
        /// Blocking receive. Returns a poll with is_some()=false once shut down.
        fn recv(self: &KeysReceiver) -> Box<KeysPoll>;

        fn is_some(self: &KeysPoll) -> bool;
        fn get_event(self: &KeysPoll) -> KeyEvent;
    }

    // ===== PsbtResult Methods =====

    extern "Rust" {
        /// Check if result is valid.
        fn is_ok(self: &PsbtResult) -> bool;

        /// Get error message (only valid if !is_ok()).
        fn get_psbt_error(self: &PsbtResult) -> String;

        /// Get transaction ID preview (only valid if is_ok()).
        fn get_txid_preview(self: &PsbtResult) -> String;

        /// Get the serialized PSBT as hex (only valid if is_ok()).
        fn bytes_hex(self: &PsbtResult) -> String;

        /// Get the serialized PSBT v2 as hex (only valid if is_ok()).
        fn bytes_v2_hex(self: &PsbtResult) -> String;
    }

    // ===== SyncEstimator Methods =====

    extern "Rust" {
        /// Create a new SyncEstimator with default EMA alpha.
        fn new_sync_estimator() -> Box<SyncEstimator>;

        /// Update estimator state from a ScanProgress notification.
        fn update(self: &mut SyncEstimator, current: u32, end: u32);

        /// Get estimated remaining seconds (0 if no estimate yet).
        fn estimate_secs(self: &SyncEstimator) -> u64;

        /// Reset estimator state (on scan start/stop).
        fn reset(self: &mut SyncEstimator);
    }
}

/// Initialize logging with the specified level.
pub fn init_logging(level: LogLevel) {
    let filter = match level {
        LogLevel::Off => log::LevelFilter::Off,
        LogLevel::Error => log::LevelFilter::Error,
        LogLevel::Warn => log::LevelFilter::Warn,
        LogLevel::Info => log::LevelFilter::Info,
        LogLevel::Debug => log::LevelFilter::Debug,
        LogLevel::Trace => log::LevelFilter::Trace,
        _ => unreachable!("CXX enum type safety guarantees no other variants exist"),
    };
    env_logger::Builder::new()
        .filter_level(filter)
        .filter_module("ureq", log::LevelFilter::Info)
        // RUST_LOG still wins, so a device that fails to enumerate can be traced without
        // a rebuild:  RUST_LOG=bwk_hwi=trace ./build/silent
        .parse_default_env()
        .init();
}

pub fn notification_to_string(notif: &Notification) -> String {
    format!("{notif:?}")
}

/// Query backend info. Returns a BackendInfo with is_ok=false on error.
pub fn get_backend_info(blindbit_url: String) -> BackendInfo {
    match bwk_sp::account::backend_info(blindbit_url) {
        Ok((info, url)) => BackendInfo {
            is_ok: true,
            error: String::new(),
            url,
            network: info.network.into(),
            height: info.height.to_consensus_u32(),
            tweaks_only: info.tweaks_only,
            tweaks_full_basic: info.tweaks_full_basic,
            tweaks_full_with_dust_filter: info.tweaks_full_with_dust_filter,
            tweaks_cut_through_with_dust_filter: info.tweaks_cut_through_with_dust_filter,
        },
        Err(e) => BackendInfo {
            is_ok: false,
            error: e.to_string(),
            url: String::new(),
            network: Network::Regtest,
            height: 0,
            tweaks_only: false,
            tweaks_full_basic: false,
            tweaks_full_with_dust_filter: false,
            tweaks_cut_through_with_dust_filter: false,
        },
    }
}

/// Validate a BIP392 Silent Payments descriptor.
pub fn validate_sp_descriptor(descriptor: String) -> bool {
    use std::str::FromStr;

    bwk_sp::bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor::from_str(&descriptor).is_ok()
}

/// Encode a GetSpDescriptor QR request as base64.
pub fn qr_get_sp_descriptor_request() -> String {
    use base64::prelude::*;

    let mut session = match keys::qr::QrSession::new() {
        Ok(session) => session,
        Err(e) => {
            log::error!("failed to create QR session: {e}");
            return String::new();
        }
    };
    match session.get_sp_descriptor(0) {
        Ok(request) => BASE64_STANDARD.encode(request.message()),
        Err(e) => {
            log::error!("failed to encode GetSpDescriptor request: {e}");
            String::new()
        }
    }
}

/// Render a GetSpDescriptor QR request.
///
/// Interim single-frame path for CreateAccount's request preview; the keys module's
/// QR modal replaces it with the multi-frame animated flow. Removing it now would
/// break its only caller for the rest of the branch.
pub fn qr_get_sp_descriptor_request_image() -> ffi::QrImageResult {
    let mut session = match keys::qr::QrSession::new() {
        Ok(session) => session,
        Err(e) => {
            return ffi::QrImageResult {
                is_ok: false,
                error: format!("failed to create QR session: {e}"),
                data: Vec::new(),
                width: 0,
                height: 0,
            }
        }
    };
    let request = match session.get_sp_descriptor(0) {
        Ok(request) => request,
        Err(e) => {
            return ffi::QrImageResult {
                is_ok: false,
                error: format!("failed to build GetSpDescriptor request: {e}"),
                data: Vec::new(),
                width: 0,
                height: 0,
            }
        }
    };
    // GetSpDescriptor has an empty body, so the whole request fits the first frame.
    match request.frame(0) {
        Ok(image) => ffi::QrImageResult {
            is_ok: true,
            error: String::new(),
            data: image.data.clone(),
            width: image.width,
            height: image.height,
        },
        Err(e) => ffi::QrImageResult {
            is_ok: false,
            error: format!("failed to render GetSpDescriptor request: {e}"),
            data: Vec::new(),
            width: 0,
            height: 0,
        },
    }
}

/// Decode a base64 SpDescriptor QR response.
///
/// Builds a session and issues its own GetSpDescriptor request so the id it expects
/// back matches what `qr_get_sp_descriptor_request` handed the signer. The stateful
/// session that spans the request/response round trip lives in the keys module; this
/// free function is the interim path for CreateAccount until it moves there.
pub fn qr_decode_sp_descriptor_response(payload: String) -> ffi::SpDescriptorResult {
    use base64::prelude::*;

    let mut session = match keys::qr::QrSession::new() {
        Ok(session) => session,
        Err(e) => {
            return ffi::SpDescriptorResult {
                is_ok: false,
                error: format!("failed to create QR session: {e}"),
                descriptor: String::new(),
                fingerprint: String::new(),
            }
        }
    };
    if let Err(e) = session.get_sp_descriptor(0) {
        return ffi::SpDescriptorResult {
            is_ok: false,
            error: format!("failed to build GetSpDescriptor request: {e}"),
            descriptor: String::new(),
            fingerprint: String::new(),
        };
    }

    let bytes = match BASE64_STANDARD.decode(payload.trim()) {
        Ok(bytes) => bytes,
        Err(e) => {
            return ffi::SpDescriptorResult {
                is_ok: false,
                error: format!("invalid base64: {e}"),
                descriptor: String::new(),
                fingerprint: String::new(),
            }
        }
    };

    match session.decode_response(&bytes) {
        Ok(keys::qr::QrResponse::SpDescriptor { descriptor, .. }) => {
            validate_sp_descriptor_origin(descriptor)
        }
        Ok(_) => ffi::SpDescriptorResult {
            is_ok: false,
            error: "QR response does not contain an SP descriptor".to_string(),
            descriptor: String::new(),
            fingerprint: String::new(),
        },
        Err(e) => ffi::SpDescriptorResult {
            is_ok: false,
            error: e.to_string(),
            descriptor: String::new(),
            fingerprint: String::new(),
        },
    }
}

/// Parses an sp() descriptor and requires an origin. Returns the descriptor
/// unchanged plus its fingerprint, or an error saying the origin is missing.
pub fn validate_sp_descriptor_origin(descriptor: String) -> ffi::SpDescriptorResult {
    use bitcoin::secp256k1::Secp256k1;
    use std::str::FromStr;

    let parsed = match bwk_sp::bwk_sign::bwk_descriptor::sp_descriptor::SpDescriptor::from_str(
        &descriptor,
    ) {
        Ok(parsed) => parsed,
        Err(e) => {
            return ffi::SpDescriptorResult {
                is_ok: false,
                error: format!("invalid sp() descriptor: {e}"),
                descriptor: String::new(),
                fingerprint: String::new(),
            }
        }
    };

    match parsed.spend_secret_key(&Secp256k1::new()) {
        Ok(Some(_)) => {
            return ffi::SpDescriptorResult {
                is_ok: false,
                error: crate::keys::error::Error::SpSpendDescriptor.to_string(),
                descriptor: String::new(),
                fingerprint: String::new(),
            }
        }
        Ok(None) => {}
        Err(e) => {
            return ffi::SpDescriptorResult {
                is_ok: false,
                error: format!("invalid sp() descriptor: {e}"),
                descriptor: String::new(),
                fingerprint: String::new(),
            }
        }
    }

    match parsed.fingerprint() {
        Some(fp) => ffi::SpDescriptorResult {
            is_ok: true,
            error: String::new(),
            descriptor,
            fingerprint: fp.to_string(),
        },
        None => ffi::SpDescriptorResult {
            is_ok: false,
            error: "sp() descriptor has no key origin".to_string(),
            descriptor: String::new(),
            fingerprint: String::new(),
        },
    }
}

/// Every fingerprint the PSBT needs a signature from, lowercase hex, deduplicated
/// and sorted. Walks bip32_derivation, tap_key_origins and the silent payment spend
/// derivation.
pub fn psbt_required_fingerprints(psbt_v2_hex: &str) -> ffi::FingerprintsResult {
    let bytes = match hex::decode(psbt_v2_hex) {
        Ok(bytes) => bytes,
        Err(e) => {
            return ffi::FingerprintsResult {
                is_ok: false,
                error: format!("invalid psbt hex: {e}"),
                fingerprints: Vec::new(),
            }
        }
    };
    let psbt = match keys::psbt::parse_v2(&bytes) {
        Ok(psbt) => psbt,
        Err(e) => {
            return ffi::FingerprintsResult {
                is_ok: false,
                error: e.to_string(),
                fingerprints: Vec::new(),
            }
        }
    };
    match keys::psbt::required_fingerprints(&psbt) {
        Ok(required) => ffi::FingerprintsResult {
            is_ok: true,
            error: String::new(),
            fingerprints: required.iter().map(ToString::to_string).collect(),
        },
        Err(e) => ffi::FingerprintsResult {
            is_ok: false,
            error: e.to_string(),
            fingerprints: Vec::new(),
        },
    }
}

/// 12 or 24 words. Any other count is an error. Fast: only draws entropy and encodes
/// a mnemonic, no password involved, so unlike the other hot functions it does not
/// block on Argon2id and is safe to call from the GUI thread.
pub fn generate_mnemonic(word_count: u32) -> ffi::MnemonicResult {
    match keys::hot::generate_mnemonic(word_count as usize) {
        Ok(mnemonic) => ffi::MnemonicResult {
            is_ok: true,
            error: String::new(),
            mnemonic: mnemonic.to_string(),
        },
        Err(e) => ffi::MnemonicResult {
            is_ok: false,
            error: e.to_string(),
            mnemonic: String::new(),
        },
    }
}

/// Validate a recipient address (SP address, legacy Bitcoin address, or hex data).
/// Returns empty string if valid, or error message if invalid.
pub fn validate_address(address: String) -> String {
    if address.is_empty() {
        return String::new();
    }
    match bwk_sp::receiver::RecipientAddress::try_from(address) {
        Ok(_) => String::new(),
        Err(e) => e.to_string(),
    }
}

/// Attempt a connect and server.version handshake over the given scheme.
fn electrum_handshake(host: &str, port: u16, scheme: ElectrumScheme) -> Result<(), String> {
    use std::time::Duration;

    let ssl = matches!(scheme, ElectrumScheme::Ssl);
    let mut client = bwk_sp::bwk::bwk_electrum::raw_client::Client::new_ssl_maybe(host, port, ssl)
        .read_timeout(Some(Duration::from_secs(5)))
        .write_timeout(Some(Duration::from_secs(5)));

    client
        .try_connect(Some(Duration::from_secs(5)))
        .map_err(|e| format!("Connection failed: {e}"))?;

    let request = bwk_sp::bwk::bwk_electrum::electrum::request::Request::version(
        "silent".to_string(),
        "1.4".to_string(),
    );
    client
        .try_send(&request)
        .map_err(|e| format!("Failed to send version request: {e}"))?;

    let response = client
        .recv_str()
        .map_err(|e| format!("No response from server: {e}"))?;

    // A server that does not speak this transport just closes the connection, and
    // bwk reads a closed connection back as an empty line, so recv_str returning
    // Ok proves nothing. Only a JSON-RPC reply means the transport is right.
    match serde_json::from_str::<serde_json::Value>(response.trim()) {
        Ok(serde_json::Value::Object(_)) => Ok(()),
        _ => Err("No electrum response from server".to_string()),
    }
}

/// Render an electrum address the way bwk expects it: the scheme rides as an
/// `ssl://` prefix on the host (see config::parse_electrum_url).
fn electrum_url(host: &str, port: u16, scheme: ElectrumScheme) -> String {
    match scheme {
        ElectrumScheme::Ssl => format!("ssl://{host}:{port}"),
        ElectrumScheme::Tcp => format!("{host}:{port}"),
    }
}

/// Test Electrum server connectivity with a server.version handshake.
///
/// When the address states a scheme it is used as-is, otherwise plaintext then
/// SSL are tried and the first to answer wins. Returns the URL that connected,
/// so the caller can store the resolved scheme.
pub fn test_electrum(address: String) -> ffi::ConnectionResult {
    log::info!("test_electrum()");

    let err_conn = |error: String| ffi::ConnectionResult {
        is_ok: false,
        error,
        url: String::new(),
    };

    let (host, port, scheme) = match bwk_sp::bwk::bwk_electrum::url::parse_electrum_url(&address) {
        Ok(parsed) => parsed,
        Err(e) => return err_conn(format!("Invalid address '{address}': {e}")),
    };
    let Some(host) = host else {
        return err_conn(format!(
            "Invalid address format '{address}': expected host:port"
        ));
    };
    let Some(port) = port else {
        // A port that is not a u16 is kept on the host rather than reported, so a
        // leftover ':' means the port is bad and not simply missing.
        return err_conn(match host.rsplit_once(':') {
            Some((_, bad)) => format!("Invalid port '{bad}' in '{address}': must be 1 to 65535"),
            None => format!("Invalid address format '{address}': expected host:port"),
        });
    };

    // An explicit scheme is honored as-is; a bare host:port probes both.
    let candidates: &[ElectrumScheme] = if address.contains("://") {
        &[scheme]
    } else {
        &[ElectrumScheme::Tcp, ElectrumScheme::Ssl]
    };

    let mut last_error = String::new();
    for candidate in candidates {
        match electrum_handshake(&host, port, *candidate) {
            Ok(()) => {
                return ffi::ConnectionResult {
                    is_ok: true,
                    error: String::new(),
                    url: electrum_url(&host, port, *candidate),
                }
            }
            Err(e) => last_error = e,
        }
    }
    err_conn(last_error)
}

/// Fetch default regtest infrastructure addresses from minta.pythcoiner.dev.
pub fn get_regtest_defaults() -> RegtestDefaults {
    let url = "http://minta.pythcoiner.dev/api/status";

    let mut response = match ureq::get(url).call() {
        Ok(r) => r,
        Err(e) => {
            log::warn!("failed to fetch regtest defaults: {e}");
            return RegtestDefaults {
                is_ok: false,
                error: format!("HTTP request failed: {e}"),
                blindbit_url: String::new(),
                electrum_url: String::new(),
            };
        }
    };

    let body: String = match response.body_mut().read_to_string() {
        Ok(s) => s,
        Err(e) => {
            log::warn!("failed to read regtest defaults response: {e}");
            return RegtestDefaults {
                is_ok: false,
                error: format!("Failed to read response: {e}"),
                blindbit_url: String::new(),
                electrum_url: String::new(),
            };
        }
    };

    let json: serde_json::Value = match serde_json::from_str(&body) {
        Ok(v) => v,
        Err(e) => {
            log::warn!("failed to parse regtest defaults JSON: {e}");
            return RegtestDefaults {
                is_ok: false,
                error: format!("JSON parse failed: {e}"),
                blindbit_url: String::new(),
                electrum_url: String::new(),
            };
        }
    };

    let blindbit_connect = json["blindbit_connect"].as_str().unwrap_or_default();
    let electrum_connect = json["electrum_connect"].as_str().unwrap_or_default();

    if blindbit_connect.is_empty() {
        return RegtestDefaults {
            is_ok: false,
            error: "Missing fields in API response".to_string(),
            blindbit_url: String::new(),
            electrum_url: String::new(),
        };
    }

    RegtestDefaults {
        is_ok: true,
        error: String::new(),
        blindbit_url: format!("http://{blindbit_connect}"),
        electrum_url: electrum_connect.to_string(),
    }
}

/// Writes the PSBT to `path` exactly as serialized. Refuses to overwrite.
pub fn sd_save_psbt(path: &str, psbt_hex: &str) -> ffi::PsbtFileResult {
    let bytes = match hex::decode(psbt_hex) {
        Ok(bytes) => bytes,
        Err(e) => return failed_psbt_file(format!("invalid psbt hex: {e}")),
    };
    match keys::sdcard::save_psbt(std::path::Path::new(path), &bytes) {
        Ok(()) => ffi::PsbtFileResult {
            is_ok: true,
            error: String::new(),
            psbt_hex: String::new(),
        },
        Err(e) => failed_psbt_file(e.to_string()),
    }
}

pub fn sd_load_psbt(path: &str) -> ffi::PsbtFileResult {
    match keys::sdcard::load_psbt(std::path::Path::new(path)) {
        Ok(bytes) => ffi::PsbtFileResult {
            is_ok: true,
            error: String::new(),
            psbt_hex: hex::encode(bytes),
        },
        Err(e) => failed_psbt_file(e.to_string()),
    }
}

fn failed_psbt_file(error: String) -> ffi::PsbtFileResult {
    ffi::PsbtFileResult {
        is_ok: false,
        error,
        psbt_hex: String::new(),
    }
}

// Re-export main types
pub use account::{new_account, Account, NotificationReceiver, Poll, PsbtResult};
pub use app_state::{
    app_active_theme, app_enabled_plugins, app_set_active_theme, app_set_plugin_enabled,
};
pub use config::{config_from_file, delete_config, list_configs, new_config, set_datadir, Config};
pub use ffi::{
    BackendInfo, CameraFrameResult, CameraListResult, FingerprintsResult, KeyDescriptor, KeyDevice,
    KeyEvent, KeyEventFlag, KeyInfo, KeyInfoResult, KeyOpResult, KeyRouteKind, KeyUnusableReason,
    KeyXpub, LiveDevice, LogLevel, MnemonicResult, Network, Notification, NotificationFlag, Output,
    PsbtFileResult, PsbtPayloadResult, PsbtValidation, QrResponseKind, RegtestDefaults,
    ScanProgress, SignResult, SigningCandidate, SigningPlan, TransactionSimulation,
    TransactionTemplate,
};
pub use hwi::{new_hwi_service, HwiPoll, HwiReceiver, HwiService};
pub use hwi_config::{
    hwi_config_network, hwi_set_config_network, BitboxSettings, HwiConfig, NoiseConfig,
    NoiseConfigData, PersistedBitboxNoiseConfig,
};
pub use keys::bridge::{
    camera_open, hot_reveal, hot_seal, hot_sign, hot_sp_descriptor, new_keys_module, qr_build_sign,
    usb_merge_signed, usb_sign_payload, CameraHandle, HotMaterial, KeysModule, KeysPoll,
    KeysReceiver, QrRequest, QrResponse, SealedHotKey,
};
pub use sync_estimator::{new_sync_estimator, SyncEstimator};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn electrum_url_renders_scheme() {
        assert_eq!(
            electrum_url("host", 50002, ElectrumScheme::Ssl),
            "ssl://host:50002"
        );
        assert_eq!(
            electrum_url("host", 50001, ElectrumScheme::Tcp),
            "host:50001"
        );
    }

    #[test]
    fn test_electrum_without_port_is_rejected() {
        let result = test_electrum("electrum.pythcoiner.dev".to_string());
        assert!(!result.is_ok);
        assert!(result.url.is_empty());
        assert!(
            result.error.contains("expected host:port"),
            "unexpected error: {}",
            result.error
        );
    }

    #[test]
    fn test_electrum_reports_an_out_of_range_port() {
        let result = test_electrum("electrum.pythcoiner.dev:500002".to_string());
        assert!(!result.is_ok);
        assert!(result.url.is_empty());
        assert!(
            result.error.contains("Invalid port '500002'"),
            "unexpected error: {}",
            result.error
        );
    }

    #[test]
    fn test_electrum_rejects_unsupported_scheme() {
        let result = test_electrum("wss://electrum.pythcoiner.dev:443".to_string());
        assert!(!result.is_ok);
        assert!(result.url.is_empty());
        assert!(
            result.error.contains("unsupported electrum scheme"),
            "unexpected error: {}",
            result.error
        );
    }

    #[test]
    fn qr_get_sp_descriptor_request_is_decodable() {
        use base64::prelude::*;
        use bwk_qr_protocol::{request, Message};

        let payload = qr_get_sp_descriptor_request();
        let bytes = BASE64_STANDARD
            .decode(payload)
            .expect("request payload should be base64");
        let message = bwk_qr_protocol::decode(&bytes).expect("request should decode");

        match message {
            Message::Request(request) => {
                assert!(matches!(request.body, request::Body::GetSpDescriptor(_)));
            }
            Message::Response(_) => panic!("expected GetSpDescriptor request"),
        }
    }

    #[test]
    fn qr_get_sp_descriptor_request_image_is_valid() {
        let result = qr_get_sp_descriptor_request_image();

        assert!(result.is_ok, "unexpected error: {}", result.error);
        assert!(result.width > 0);
        assert!(result.height > 0);
        assert_eq!(result.data.len(), (result.width * result.height) as usize);
    }

    #[test]
    fn qr_decode_sp_descriptor_response_extracts_descriptor() {
        use base64::prelude::*;
        use bitcoin::bip32::{DerivationPath, Xpriv};
        use bitcoin::secp256k1::Secp256k1;
        use bitcoin::{Network, PublicKey};
        use bwk_qr_protocol::{response, RequestId, Response};
        use std::str::FromStr;

        let secp = Secp256k1::new();
        let scan_xprv = Xpriv::new_master(Network::Bitcoin, &[0x11; 64]).expect("valid seed");
        let spend_xprv = Xpriv::new_master(Network::Bitcoin, &[0x22; 64]).expect("valid seed");
        let fp = scan_xprv.fingerprint(&secp);
        let spend_child = spend_xprv
            .derive_priv(
                &secp,
                &DerivationPath::from_str("m/0h").expect("valid path"),
            )
            .expect("derive spend child");
        let spend_pubkey = PublicKey::from_private_key(&secp, &spend_child.to_priv());
        let descriptor = format!("sp([{fp}/352h/0h/0h]{scan_xprv}/0h,{spend_pubkey})");

        let response = Response {
            id: RequestId([0, 1]),
            body: response::Body::SpDescriptor(response::SpDescriptor {
                descriptor: descriptor.to_string(),
            }),
        };
        let bytes = bwk_qr_protocol::encode_response(&response).expect("response should encode");
        let result = qr_decode_sp_descriptor_response(BASE64_STANDARD.encode(bytes));

        assert!(result.is_ok, "unexpected error: {}", result.error);
        assert_eq!(result.descriptor, descriptor);
    }

    #[test]
    fn qr_decode_sp_descriptor_response_rejects_request() {
        let result = qr_decode_sp_descriptor_response(qr_get_sp_descriptor_request());

        assert!(!result.is_ok);
        assert_eq!(result.error, "QR payload is a request, not a response");
        assert!(result.descriptor.is_empty());
    }
}
