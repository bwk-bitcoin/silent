//! Silent - Bitcoin Silent Payment wallet backed by BlindBit
//!
//! This library provides C++ bindings for the bwk-sp Silent Payment implementation.

pub mod account;
pub mod app_state;
pub mod config;
pub mod hwi;
pub mod hwi_config;
pub mod sync_estimator;

use bwk_sp::bwk::ElectrumScheme;

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
    }

    // ===== Utility Functions =====

    extern "Rust" {
        /// Initialize logging with the specified level.
        /// Call this once at application startup.
        fn init_logging(level: LogLevel);

        /// Generate a new 12-word BIP39 mnemonic.
        fn generate_mnemonic() -> String;
        fn notification_to_string(notif: &Notification) -> String;

        /// Query backend server info (network, height, capabilities).
        /// Blocking HTTP call - may take up to 30s on timeout.
        fn get_backend_info(blindbit_url: String) -> BackendInfo;

        /// Validate a BIP39 mnemonic string.
        fn validate_mnemonic(mnemonic: String) -> bool;

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
    }

    // ===== Config Methods =====

    extern "Rust" {
        /// Create a new config.
        fn new_config(
            account_name: String,
            network: Network,
            mnemonic: String,
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

        /// Get mnemonic.
        fn get_mnemonic(self: &Config) -> String;

        /// Set mnemonic.
        fn set_mnemonic(self: &mut Config, mnemonic: String);

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

    // ===== PsbtResult Methods =====

    extern "Rust" {
        /// Check if result is valid.
        fn is_ok(self: &PsbtResult) -> bool;

        /// Get error message (only valid if !is_ok()).
        fn get_psbt_error(self: &PsbtResult) -> String;

        /// Get transaction ID preview (only valid if is_ok()).
        fn get_txid_preview(self: &PsbtResult) -> String;
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

/// Generate a new 12-word BIP39 mnemonic.
pub fn generate_mnemonic() -> String {
    let mut entropy = [0u8; 16]; // 128 bits = 12 words
    getrandom::getrandom(&mut entropy).expect("failed to generate random entropy");
    bwk_sp::receiver::bip39::Mnemonic::from_entropy(&entropy)
        .expect("mnemonic generation from 128-bit entropy should not fail")
        .to_string()
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

/// Validate a BIP39 mnemonic string.
pub fn validate_mnemonic(mnemonic: String) -> bool {
    bwk_sp::receiver::bip39::Mnemonic::parse(&mnemonic).is_ok()
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

    let (host, port, scheme) = match bwk_sp::bwk::parse_electrum_url(&address) {
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

// Re-export main types
pub use account::{new_account, Account, NotificationReceiver, Poll, PsbtResult};
pub use app_state::{
    app_active_theme, app_enabled_plugins, app_set_active_theme, app_set_plugin_enabled,
};
pub use config::{config_from_file, delete_config, list_configs, new_config, set_datadir, Config};
pub use ffi::{
    BackendInfo, LogLevel, Network, Notification, NotificationFlag, Output, PsbtValidation,
    RegtestDefaults, TransactionSimulation, TransactionTemplate,
};
pub use hwi::{new_hwi_service, HwiPoll, HwiReceiver, HwiService};
pub use hwi_config::{
    hwi_config_network, hwi_set_config_network, BitboxSettings, HwiConfig, NoiseConfig,
    NoiseConfigData, PersistedBitboxNoiseConfig,
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
}
