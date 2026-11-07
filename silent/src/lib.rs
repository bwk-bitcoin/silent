//! Silent - Bitcoin Silent Payment wallet backed by BlindBit
//!
//! This library provides C++ bindings for the bwk-sp Silent Payment implementation.

pub mod account;
mod account_storage;
pub mod app_state;
pub mod backup;
pub mod bbqr;
pub mod camera;
pub mod config;
pub mod coordination;
pub mod endpoint;
pub mod error;
pub mod feed;
pub mod hwi;
pub mod hwi_config;
pub mod image;
pub mod keys;
pub mod money;
pub mod musig;
pub mod plugin_admission;
pub mod plugin_bundle;
pub mod policy;
pub mod psbt_transfer;
#[cfg(all(test, unix))]
mod removal_process_tests;
pub mod saved_psbt;
mod services;
pub mod spending;
pub mod state_crypt;
pub mod storage;
pub mod sync_estimator;
pub mod tx_details;
mod tx_preview;

use bwk_sp::bwk::bwk_electrum::url::ElectrumScheme;

#[cxx::bridge]
mod ffi {
    // ===== Shared Enums =====

    #[derive(Debug, Clone, Copy)]
    pub enum KeyFileFailure {
        None,
        Read,
        Newer,
        Network,
        Invalid,
    }
    #[derive(Debug, Clone, Copy)]
    pub enum PluginMetadataKind {
        Text,
        Number,
        Flag,
        Strings,
    }
    #[derive(Debug, Clone, Copy)]
    #[repr(u8)]
    pub enum PluginRemovalPhase {
        Stopping,
        PrivateData,
        Bundle,
        Bookkeeping,
    }
    #[derive(Debug, Clone, Copy)]
    pub enum BackendErrorKind {
        Other,
        None,
        InvalidInput,
        Storage,
        Network,
        Electrum,
        Backend,
        Signing,
        Broadcast,
        InputConflict,
        AddressReuse,
    }
    #[derive(Debug, Clone, Copy)]
    pub enum EndpointPhase {
        Ready,
        Draining,
        Complete,
    }

    /// Bitcoin network types.
    #[derive(Debug, Clone, Copy)]
    pub enum Network {
        Regtest,
        Signet,
        Testnet,
        Bitcoin,
    }

    /// A backend server an account connects to.
    #[derive(Debug, Clone, Copy)]
    pub enum BackendServer {
        Blindbit,
        Electrum,
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

    /// Why a device request failed, beyond its message.
    #[derive(Debug, Clone, Copy)]
    pub enum HwiErrorKind {
        Other,
        /// The device has no MuSig2 session for the PSBT: restart from round 1.
        MusigSession,
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
        UnsupportedMusig,
        UnsupportedOperation,
    }

    /// Kind of a decoded and validated QR response.
    #[derive(Debug, Clone, Copy)]
    pub enum QrResponseKind {
        Xpubs,
        Registration,
        AddressVerified,
        SignedPsbt,
        SpAccount,
        MusigPrepared,
        MusigSignedPsbt,
    }

    /// MuSig2 signing round of a PSBT.
    #[derive(Debug, Clone, Copy)]
    pub enum MusigRound {
        NotMusig,
        Nonces,
        PartialSigs,
        Ready,
    }

    /// Why a QR response was refused: the three failures the UI words on their own
    /// (a request id mismatch, a signatures-only answer, a lost MuSig2 session), and
    /// every other one, which only surfaces as free text.
    #[derive(Debug, Clone, Copy)]
    pub enum QrErrorKind {
        Other,
        RequestIdMismatch,
        SignaturesUnsupported,
        /// The device lost or refused its MuSig2 session (response error 0x0D).
        MusigSession,
    }

    // ===== Shared Structs =====

    #[derive(Debug, Clone, Default)]
    pub struct RetryDecision {
        pub schedule: bool,
        pub give_up: bool,
        pub losses: u32,
        pub delay_ms: u32,
    }

    #[derive(Debug, Clone, Default)]
    pub struct FeedQuote {
        pub available: bool,
        pub value: f64,
    }
    #[derive(Debug, Clone)]
    pub struct FeedPrice {
        pub source: String,
        pub currency: String,
        pub quote: FeedQuote,
    }
    #[derive(Debug, Clone)]
    pub struct FeedSelection {
        pub preferences: RustFeePreferences,
        pub default_rate: FeedQuote,
        pub low: FeedQuote,
        pub medium: FeedQuote,
        pub fast: FeedQuote,
    }
    #[derive(Debug, Clone, Default)]
    pub struct FeedSourceState {
        pub currencies: Vec<String>,
        pub has_default: bool,
        pub has_smart: bool,
    }

    #[derive(Debug, Clone, Default)]
    pub struct PluginMetadataField {
        pub key: String,
        pub kind: PluginMetadataKind,
        pub text: String,
        pub number: f64,
        pub flag: bool,
        pub items: Vec<String>,
    }
    #[derive(Debug, Clone)]
    pub struct PluginMetadata {
        pub id: String,
        pub fields: Vec<PluginMetadataField>,
    }

    #[derive(Debug, Clone)]
    pub struct PluginRemovalEntry {
        pub plugin_id: String,
        pub phase: PluginRemovalPhase,
    }
    #[derive(Debug, Clone)]
    pub struct PluginRemovalListResult {
        pub is_ok: bool,
        pub error: String,
        pub entries: Vec<PluginRemovalEntry>,
        pub enabled_plugins: Vec<String>,
        pub access: Vec<PluginAccess>,
    }
    #[derive(Debug, Clone)]
    pub struct PluginGrantedAccess {
        pub account: bool,
        pub keys: bool,
        pub state: bool,
    }

    #[derive(Debug, Clone, Default)]
    pub struct AmountParseResult {
        pub is_ok: bool,
        pub sats: u64,
    }

    #[derive(Debug, Clone, Default)]
    pub struct FeeParseResult {
        pub is_ok: bool,
        pub fee: u64,
        pub fee_rate: f64,
    }

    #[derive(Debug, Clone, Default)]
    pub struct SendAmounts {
        pub is_ok: bool,
        pub input_total: u64,
        pub sending_total: u64,
        pub remaining: u64,
        pub change: u64,
        pub transaction_total: u64,
    }

    #[derive(Debug, Clone, Default)]
    pub struct SpCreationRequest {
        pub account_name: String,
        pub network: Network,
        pub descriptor: String,
        pub taproot_descriptor: String,
        pub segwit_descriptor: String,
        pub blindbit_url: String,
        pub electrum_url: String,
    }

    #[derive(Debug, Clone, Default)]
    pub struct MusigCreationRequest {
        pub account_name: String,
        pub network: Network,
        pub descriptor: String,
        pub electrum_url: String,
        pub verify_certificate: bool,
    }

    #[derive(Debug, Clone, Default)]
    pub struct AccountCreationResult {
        pub is_ok: bool,
        pub error: String,
        pub preference_error: String,
    }

    /// Global fee mode (Manual=0, Default=1, Smart=2) and selected provider.
    #[derive(Debug, Clone, Default)]
    struct RustFeePreferences {
        mode: u8,
        source: String,
    }

    /// Notification from the account scanner.
    #[derive(Debug, Clone)]
    pub struct Notification {
        pub flag: NotificationFlag,
        pub payload: String,
        pub current: u32,
        pub end: u32,
    }

    /// A coin entry from the wallet.
    #[derive(Debug, Clone)]
    pub struct RustCoin {
        pub outpoint: String,
        pub value: u64,
        pub height: u32,
        /// Confirming block time, zero until known.
        pub timestamp: u64,
        pub label: String,
        pub spent: bool,
        pub account_type: String,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum SpendLockKind {
        None,
        AbsoluteHeight,
        AbsoluteTime,
        RelativeBlocks,
        RelativeTime512,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum SpendMaturity {
        Mature,
        Immature,
        Unknown,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum SpendAvailability {
        Available,
        Immature,
        Unknown,
        Spent,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum SpendCoinStatus {
        Unconfirmed,
        ConfirmedUnverified,
        Confirmed,
        BeingSpent,
        Spent,
    }

    #[derive(Debug, Clone)]
    pub struct RustSpendLock {
        pub kind: SpendLockKind,
        pub value: u32,
    }

    #[derive(Debug, Clone)]
    pub struct RustSpendVariant {
        pub path_id: String,
        pub keychain: u32,
        pub derivation_index: u32,
        pub display: String,
        pub psbt_v2_hex: String,
        pub descriptor: String,
        pub sp_spend_key: String,
        pub required_keys: Vec<String>,
        pub required_preimages: Vec<String>,
        pub satisfaction_wu: u64,
        pub absolute_lock: RustSpendLock,
        pub relative_lock: RustSpendLock,
        pub maturity: SpendMaturity,
        pub availability: SpendAvailability,
    }

    #[derive(Debug, Clone)]
    pub struct RustSpendCoin {
        pub outpoint: String,
        pub account_type: String,
        pub network: Network,
        pub value: u64,
        pub height: u64,
        pub has_height: bool,
        pub label: String,
        pub status: SpendCoinStatus,
        pub variants: Vec<RustSpendVariant>,
    }

    #[derive(Debug, Clone)]
    pub struct SpendCoinsResult {
        pub is_ok: bool,
        pub error: String,
        pub coins: Vec<RustSpendCoin>,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum SigningStage {
        Signatures,
        MusigNonces,
        MusigSignatures,
        SpContributions,
        VerifySpContributions,
        CompleteSpOutputs,
        SpCompleteAndSign,
        Release,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum SigningProgress {
        Partial,
        StageComplete,
        Complete,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum SpendError {
        Other,
        Unsupported,
        InvalidRequest,
        Integrity,
        LostSession,
        Permission,
        Cancelled,
    }

    #[derive(Debug, Clone)]
    pub struct RustSelectedInput {
        pub input_index: u32,
        pub network: Network,
        pub variant: RustSpendVariant,
    }

    #[derive(Debug, Clone)]
    pub struct CoordinationResult {
        pub is_ok: bool,
        pub error: String,
        pub kind: SpendError,
        pub psbt_v2_hex: String,
        pub progress: SigningProgress,
        pub tx_hex: String,
        pub txid: String,
        pub session_id: String,
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
    #[derive(Debug, Clone, Default)]
    pub struct RustTx {
        pub txid: String,
        pub direction: String,
        pub amount: u64,
        pub amount_known: bool,
        pub fee: u64,
        pub fee_known: bool,
        pub height: u32,
        pub timestamp: u64,
        pub label: String,
        pub status: String,
    }

    #[derive(Debug, Clone, Default)]
    pub struct RustTxIo {
        pub index: u32,
        pub outpoint: String,
        pub value: u64,
        pub value_known: bool,
        pub address: String,
        pub script: String,
        pub is_mine: bool,
        pub ownership_known: bool,
        pub label: String,
    }

    #[derive(Debug, Clone)]
    pub struct LabelEditResult {
        pub is_ok: bool,
        pub error: String,
        /// The label held in RAM changed, even when saving it failed.
        pub changed: bool,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum LabelKind {
        Address,
        Outpoint,
        Transaction,
    }

    #[derive(Debug, Clone)]
    pub struct LabelEntry {
        pub kind: LabelKind,
        /// The address, the `txid:vout` outpoint or the txid.
        pub id: String,
        pub text: String,
    }

    /// Every label the account holds in RAM, saved or not.
    #[derive(Debug, Clone)]
    pub struct LabelSnapshot {
        pub is_ok: bool,
        pub error: String,
        pub labels: Vec<LabelEntry>,
    }

    /// Coins, balance and history read together from the account's stores.
    #[derive(Debug, Clone)]
    pub struct FundsSnapshot {
        pub is_ok: bool,
        pub error: String,
        /// Spent coins included.
        pub coins: Vec<RustCoin>,
        /// The outpoints of the unspent coins a spend path can spend now.
        pub spendable: Vec<String>,
        pub balance: CoinState,
        pub history: Vec<RustTx>,
        /// Every coin's funding transaction is in the history: false while a store
        /// writes a coin before its transaction.
        pub history_complete: bool,
    }

    /// A kind of address the account receives on.
    #[derive(Debug, Clone)]
    pub struct ReceiveKind {
        /// "sp", "segwit" or "taproot".
        pub id: String,
        /// One static address, handed out again and again.
        pub reusable: bool,
    }

    /// A receive address the account handed out, with what a device needs to show it.
    #[derive(Debug, Clone)]
    pub struct ReceiveAddress {
        pub kind: String,
        pub address: String,
        pub descriptor: String,
        pub path: String,
    }

    #[derive(Debug, Clone)]
    pub struct ReceiveAddressResult {
        pub is_ok: bool,
        pub error: String,
        pub address: ReceiveAddress,
    }

    /// A transaction template checked against the account's coins.
    #[derive(Debug, Clone)]
    pub struct CraftOutcome {
        /// Buildable as it is: inputs given, funds enough, recipients valid.
        pub complete: bool,
        pub error: String,
        /// The template, with the MAX output's amount resolved once complete.
        pub transaction: TransactionTemplate,
    }

    /// The unsigned PSBTv2 of a complete template.
    #[derive(Debug, Clone)]
    pub struct UnsignedPsbt {
        pub is_ok: bool,
        pub error: String,
        pub psbt: Vec<u8>,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum BroadcastOutcome {
        /// The server took the transaction.
        Accepted,
        /// The transaction did not reach the network, or the server refused it.
        Rejected,
        /// Sent, but whether the server took it is not known.
        Unknown,
    }

    #[derive(Debug, Clone)]
    pub struct BroadcastReport {
        pub outcome: BroadcastOutcome,
        pub txid: String,
        pub kind: BackendErrorKind,
        pub error: String,
        /// After acceptance: what the account failed to record locally.
        pub warnings: Vec<String>,
    }

    /// One endpoint to save, as Settings edits it.
    #[derive(Debug, Clone)]
    pub struct EndpointSetting {
        pub server: BackendServer,
        pub url: String,
        pub verify_certificate: bool,
    }

    #[derive(Debug, Clone, Default)]
    pub struct RustTxDetails {
        pub is_ok: bool,
        pub error: String,
        /// Missing prevouts or an offline lookup do not invalidate known transaction data.
        pub warning: String,
        pub tx: RustTx,
        pub network: Network,
        pub version: i32,
        pub lock_time: u32,
        pub weight: u64,
        pub vsize: u64,
        pub inputs: Vec<RustTxIo>,
        pub outputs: Vec<RustTxIo>,
        pub input_total: u64,
        pub input_total_known: bool,
        pub output_total: u64,
        pub fee: u64,
        pub fee_known: bool,
        pub fee_rate: f64,
        pub chain_tip: u32,
        pub chain_tip_known: bool,
    }

    #[derive(Debug, Clone, Default)]
    pub struct EndpointResult {
        pub kind: BackendErrorKind,
        /// Reopen succeeded; transport confirmation still arrives through notifications.
        pub connection_pending: bool,
        pub is_ok: bool,
        pub saved: bool,
        pub error: String,
        pub url: String,
        pub verify_certificate: bool,
    }

    /// Transaction output specification.
    #[derive(Debug, Clone, Default, Serialize, Deserialize)]
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
    #[derive(Debug, Clone, Default, Serialize, Deserialize)]
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
    #[derive(Debug, Clone, Default)]
    pub struct TransactionSimulation {
        pub kind: BackendErrorKind,
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
        pub selected_values: Vec<u64>,
        pub recipient_amounts: Vec<u64>,
        pub sending_total: u64,
        pub change: u64,
        pub transaction_total: u64,
    }

    /// Result of a transaction operation (sign, broadcast).
    #[derive(Debug, Clone)]
    pub struct TxResult {
        pub kind: BackendErrorKind,
        /// Whether the operation succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// Result value: signed tx hex or txid (empty on error).
        pub value: String,
    }

    #[derive(Debug, Clone, Default)]
    pub struct MusigFinalizeResult {
        pub is_ok: bool,
        pub error: String,
        pub psbt_v2_hex: String,
        pub tx_hex: String,
        /// Compatibility with callers that used TxResult::value.
        pub value: String,
        pub save_error: String,
    }

    #[derive(Debug, Clone, Default)]
    pub struct MusigAttemptSnapshot {
        pub is_ok: bool,
        pub error: String,
        pub save_error: String,
        pub finalize_error: String,
        pub psbt_v2_hex: String,
        pub tx_hex: String,
        pub record_id: String,
        pub signers: Vec<String>,
        pub reset_pending: bool,
        /// A nonce session was durably taken from cache for this attempt: Reset
        /// exists from here on, even when nothing was published with it.
        pub nonces_allocated: bool,
    }

    #[derive(Debug, Clone, Default, Serialize, Deserialize)]
    pub struct SavedPsbtAttempt {
        pub attempt_id: u64,
        pub baseline_psbt_v2_hex: String,
        pub current_psbt_v2_hex: String,
    }

    #[derive(Debug, Clone, Default)]
    pub struct SavedPsbtReset {
        pub is_ok: bool,
        pub error: String,
        pub save_error: String,
        pub psbt_v2_hex: String,
        pub reset_pending: bool,
    }

    #[derive(Debug, Clone, Default, Serialize, Deserialize)]
    pub struct SavedPsbtRecord {
        pub id: String,
        pub tx_template: TransactionTemplate,
        pub spend: MusigSpendLeaf,
        pub original_psbt_v2_hex: String,
        pub current_psbt_v2_hex: String,
        #[serde(default)]
        pub attempt_id: u64,
        #[serde(default)]
        pub baseline_psbt_v2_hex: String,
        #[serde(default)]
        pub archived_attempts: Vec<SavedPsbtAttempt>,
        #[serde(default)]
        pub reset_pending: bool,
        pub finalized_tx_hex: String,
        pub broadcast: bool,
        pub label: String,
        pub created_at: u64,
        pub updated_at: u64,
    }

    #[derive(Debug, Clone, Default)]
    pub struct SavedPsbtInput {
        pub outpoint: String,
        pub value: u64,
    }

    #[derive(Debug, Clone, Default)]
    pub struct SavedPsbtEntry {
        pub record: SavedPsbtRecord,
        pub txid: String,
        pub frozen_template: TransactionTemplate,
        pub inputs: Vec<SavedPsbtInput>,
        pub input_total: u64,
        pub output_total: u64,
        pub confirmed_height: u32,
        pub in_history: bool,
        pub conflicting_txids: Vec<String>,
        pub warning: String,
        pub signatures_complete: u32,
        pub signatures_required: u32,
        pub signatures_known: bool,
    }

    #[derive(Debug, Clone, Default)]
    pub struct SavedPsbtList {
        pub is_ok: bool,
        pub error: String,
        pub save_error: String,
        pub entries: Vec<SavedPsbtEntry>,
    }

    #[derive(Debug, Clone, Default)]
    pub struct SavedPsbtRestore {
        pub is_ok: bool,
        pub error: String,
        pub save_error: String,
        pub entry: SavedPsbtEntry,
    }

    #[derive(Debug, Clone, Default)]
    pub struct SavedPsbtMutation {
        pub is_ok: bool,
        pub error: String,
        pub save_error: String,
        pub record_id: String,
    }

    /// A PSBT as base64 text, to save to a `.psbt` file or copy to the clipboard.
    #[derive(Debug, Clone, Default)]
    pub struct PsbtExport {
        pub is_ok: bool,
        pub error: String,
        pub base64: String,
        /// Empty while a silent payment output has no script yet.
        pub txid: String,
    }

    /// An imported PSBT the account can sign: its PSBTv2 and the transaction it states.
    #[derive(Debug, Clone, Default)]
    pub struct PsbtImport {
        pub is_ok: bool,
        pub error: String,
        pub psbt: Vec<u8>,
        /// Its recipients, absolute fee and inputs.
        pub tx_template: TransactionTemplate,
    }

    /// An imported PSBT saved, or merged into the saved PSBT of the same transaction.
    #[derive(Debug, Clone, Default)]
    pub struct SavedPsbtImport {
        pub is_ok: bool,
        pub error: String,
        pub save_error: String,
        pub record_id: String,
        pub merged: bool,
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

    /// One payload a QR scan joined, its bytes as framed.
    #[derive(Debug, Clone)]
    pub struct QrPayload {
        pub data: Vec<u8>,
    }

    /// What one camera frame gave a BBQR scan: the payloads it completed, and the parts
    /// seen of the total of the payload being joined (0 of 0 while none is).
    #[derive(Debug, Clone)]
    pub struct BbqrScanResult {
        pub is_ok: bool,
        pub error: String,
        pub payloads: Vec<QrPayload>,
        pub seen: u32,
        pub total: u32,
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

    /// The chain tip height an Electrum server follows.
    #[derive(Debug, Clone)]
    pub struct ElectrumTipResult {
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        pub height: u32,
    }

    /// Result of a connection test.
    #[derive(Debug, Clone)]
    pub struct ConnectionResult {
        pub kind: BackendErrorKind,
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
        pub kind: BackendErrorKind,
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
        /// Capability bits: 0 ordinary, 1 Taproot, 2 silent payments, 3 MuSig2. Zero
        /// unless the device is Supported.
        pub capabilities: u32,
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
        /// Error events only, `Other` otherwise.
        pub error_kind: HwiErrorKind,
        /// WalletRegistered only: the device's proof, hex, empty when it returned none.
        pub hmac: String,
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
        /// Signs MuSig2: a hot secret, or a recorded or live device reporting the
        /// MuSig2 capability.
        pub can_musig: bool,
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
        /// Firmware version, empty for a USB device.
        pub version: String,
        /// Last reported bwk-qr capability bits, including ordinary/Taproot for USB.
        pub capabilities: u32,
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
        /// Firmware version, empty for a USB device.
        pub version: String,
        /// Explicit ordinary, Taproot, silent payments and MuSig2 capability bits.
        pub capabilities: u32,
    }

    /// A cached extended public key at a derivation path for a registered key.
    #[derive(Debug, Clone)]
    pub struct KeyXpub {
        pub fingerprint: String,
        pub path: String,
        pub xpub: String,
        /// An account or a registered descriptor carried it, even one since deleted.
        pub used: bool,
    }

    /// Descriptor and derivation path needed to verify a generated address on a
    /// signing device. Empty when `address` cannot be resolved (e.g. the silent
    /// payment address itself, which is not derived at a BIP32 path).
    #[derive(Debug, Clone)]
    pub struct AddressVerifyInfo {
        pub descriptor: String,
        pub path: String,
    }

    /// A descriptor a key participates in.
    #[derive(Debug, Clone)]
    pub struct KeyDescriptor {
        pub fingerprint: String,
        pub descriptor: String,
        /// The sha256-prefix alias the device knows this descriptor by.
        pub alias: String,
        pub registered: bool,
        /// The proof the device returned for the registration, hex, empty when none.
        pub hmac: String,
    }

    /// Result of a keys module operation carrying no response data.
    #[derive(Debug, Clone)]
    pub struct KeyOpResult {
        /// Whether the operation succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
    }

    /// A QR protocol message, as the bytes a transport frames.
    #[derive(Debug, Clone)]
    pub struct QrMessageResult {
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        pub payload: Vec<u8>,
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
        /// The key already gave its share for the PSBT's current MuSig2 round.
        pub musig_done: bool,
        pub nonces_done: bool,
        pub signatures_done: bool,
        /// Every MuSig policy this key still owes nonces for has a usable cached
        /// session. Always true when nothing is pending.
        pub cache_ready: bool,
        /// The key's route supports transaction-independent cache preparation.
        pub cache_supported: bool,
    }

    /// One pending signer's plan route, for the cached signing allocation to bind
    /// every required session.
    #[derive(Debug, Clone)]
    pub struct QrMusigSignerRoute {
        pub fingerprint: String,
        pub route: KeyRouteKind,
    }

    /// The outcome of a cached MuSig QR exchange: the verified PSBT as v2 hex after
    /// a signing answer, or why the exchange failed.
    #[derive(Debug, Clone)]
    pub struct QrMusigCacheResult {
        pub is_ok: bool,
        pub error: String,
        /// The session the attempt was bound to is gone and signing must restart.
        pub musig_session: bool,
        pub psbt_v2_hex: String,
    }

    #[derive(Debug, Clone)]
    pub struct MusigSessionCreateResult {
        pub is_ok: bool,
        pub error: String,
        pub wrong_password: bool,
    }

    #[derive(Debug, Clone)]
    pub struct HotCachedNonceResult {
        pub is_ok: bool,
        pub error: String,
        pub needs_password: bool,
        pub musig_session: bool,
        pub psbt_v2_hex: String,
    }

    #[derive(Debug, Clone)]
    pub struct LedgerNonceSignResult {
        pub is_ok: bool,
        pub error: String,
        pub musig_session: bool,
        pub need_fetch: bool,
        pub musig: bool,
        pub from_cache: bool,
        pub psbt_v2_hex: String,
        pub psbt_base64: String,
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
        /// The password did not open the hot secret. Only `hot_reveal` sets it.
        pub wrong_password: bool,
    }

    /// Result of a hot signing operation.
    #[derive(Debug, Clone)]
    pub struct SignResult {
        /// Whether the signing operation succeeded.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// The password did not open the hot secret. Only `hot_sign` sets it.
        pub wrong_password: bool,
        /// The hot key has no MuSig2 session for this PSBT: restart from round 1.
        pub musig_session: bool,
        /// The signed PSBT, serialized v2 and hex encoded. Empty on error.
        pub psbt_v2_hex: String,
    }

    /// Result of `hot_sp_descriptor`.
    #[derive(Debug, Clone)]
    pub struct HotXpubResult {
        /// Whether the xpub was derived.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// The password did not open the hot secret.
        pub wrong_password: bool,
        /// The xpub at the asked path. Empty on error.
        pub xpub: String,
    }

    pub struct HotSpDescriptorResult {
        /// Whether the descriptor was derived.
        pub is_ok: bool,
        /// Error message (empty if is_ok is true).
        pub error: String,
        /// The password did not open the hot secret.
        pub wrong_password: bool,
        /// The sp() descriptor, origin included. Empty on error.
        pub descriptor: String,
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

    #[derive(Debug, Clone)]
    pub struct CameraListResult {
        pub is_ok: bool,
        pub error: String,
        pub names: Vec<String>,
        /// Parallel to `names`: the index to pass to camera_open.
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

    /// The QR display steppers' levels, 1 lowest to 5 highest.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct QrDisplayLevels {
        pub density: u8,
        pub contrast: u8,
        pub speed: u8,
    }

    /// How the app state unlocks.
    #[derive(Debug, Clone)]
    pub struct StateUnlockInfo {
        /// The state has an unlock key: false before it is created.
        pub exists: bool,
        /// A signing device, else a password.
        pub device: bool,
        /// The path to ask the device for its key at, "m/138'/1936225652'/<index>'".
        pub path: String,
        /// Set when the check file cannot be read.
        pub error: String,
    }

    /// What installing a dropped plugin gave.
    #[derive(Debug, Clone)]
    pub struct PluginInstallResult {
        pub is_ok: bool,
        pub plugin_id: String,
        pub error: String,
    }

    /// One translation catalog of a plugin, by file name (`silent_en.lang`).
    #[derive(Debug, Clone)]
    pub struct PluginCatalog {
        pub name: String,
        pub text: String,
    }

    /// An installed plugin's library and catalogs, opened from the state.
    #[derive(Debug, Clone)]
    pub struct PluginLibraryResult {
        pub is_ok: bool,
        /// The library's file name, for its type.
        pub library_name: String,
        pub library: Vec<u8>,
        pub catalogs: Vec<PluginCatalog>,
        pub error: String,
    }

    /// A device key's index, as `state_draw_index` answers it.
    #[derive(Debug, Clone)]
    pub struct StateIndexResult {
        pub is_ok: bool,
        /// Below 2^31, set only when `is_ok`.
        pub index: u32,
        pub error: String,
    }

    /// The user's answer to one use an external plugin asked for.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum PluginGrant {
        /// Not asked yet: the host asks on the plugin's first use.
        Unasked,
        Allowed,
        Denied,
    }

    /// What one external plugin library may use, by its sha256.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct PluginAccess {
        pub plugin_id: String,
        /// The sha256 of the plugin library, hex.
        pub hash: String,
        /// May call `IAccount`, on every account.
        pub account: PluginGrant,
        /// May call `IKeys`.
        pub keys: PluginGrant,
        /// May open the plugin's own saved state.
        pub state: PluginGrant,
    }

    /// The answers for one plugin library, read with the plugin's storage epoch when a
    /// grant request starts. Refused while the plugin's removal is pending.
    #[derive(Debug, Clone)]
    pub struct PluginAccessRequest {
        pub is_ok: bool,
        pub error: String,
        /// Given back to `app_set_plugin_access`: a removal since makes the save fail.
        pub storage_epoch: u64,
        /// In-process publication revision, checked from memory when consuming the answer.
        pub grant_revision: u64,
        /// Persisted publication version, compared under both locks on a prompt save.
        pub grant_version: u64,
        /// Every grant unasked when none is kept for the library.
        pub access: PluginAccess,
    }

    /// Every answer kept for one plugin, read with its storage epoch when Settings
    /// shows them. Refused while the plugin's removal is pending.
    #[derive(Debug, Clone)]
    pub struct PluginAccessHistory {
        pub is_ok: bool,
        pub error: String,
        /// Given back to `app_set_plugin_access`: a removal since makes the save fail.
        pub storage_epoch: u64,
        /// One entry per library hash, in the order they were answered.
        pub entries: Vec<PluginAccess>,
    }

    #[derive(Debug, Clone)]
    pub struct PsbtFileResult {
        pub is_ok: bool,
        pub error: String,
        /// The PSBT read from disk, hex encoded. Empty on save and on error.
        pub psbt_hex: String,
    }

    /// Result of writing a descriptor's BIP138 backup.
    #[derive(Debug, Clone)]
    pub struct DescriptorBackupResult {
        pub is_ok: bool,
        pub error: String,
        /// The key expressions left out of the encryption keys: their holders cannot
        /// decrypt the backup with them.
        pub excluded: Vec<String>,
    }

    /// Result of validating a musig() descriptor.
    #[derive(Debug, Clone)]
    pub struct MusigDescriptorResult {
        pub is_ok: bool,
        pub error: String,
        /// Standard BIP-390 text with checksum. Empty on error.
        pub descriptor: String,
    }

    /// Result of saving the Electrum endpoint of a MuSig2 account.
    #[derive(Debug, Clone)]
    pub struct MusigElectrumUrlResult {
        /// The url was written to the config.
        pub saved: bool,
        /// Kept for existing callers. Saving never starts a listener.
        pub started: bool,
    }

    /// Signing progress of one cosigner on a PSBT.
    #[derive(Debug, Clone)]
    pub struct MusigCosignerStatus {
        pub fingerprint: String,
        pub nonces: u32,
        pub partial_sigs: u32,
        pub expected: u32,
    }

    /// Signing progress of a PSBT.
    #[derive(Debug, Clone)]
    pub struct MusigRoundStatus {
        pub is_ok: bool,
        pub error: String,
        pub round: MusigRound,
        pub cosigners: Vec<MusigCosignerStatus>,
    }

    /// One way to spend a musig account's coins: the key path or one tap leaf
    /// (bwk `SpendLeaf`). Picked before round 1 (decision 15).
    #[derive(Debug, Clone, Default, Serialize, Deserialize)]
    pub struct MusigSpendLeaf {
        /// The key path (internal key); leaf_hash is empty.
        pub key_path: bool,
        /// Hex tap leaf hash.
        pub leaf_hash: String,
        /// Master fingerprints of the keys that sign it.
        pub fingerprints: Vec<String>,
        /// `older()` blocks, 0 for none.
        pub relative: u32,
        /// `after()` height, 0 for none.
        pub absolute: u32,
        /// Every given coin's timelock on it is met at the current tip.
        pub spendable: bool,
        /// Witness weight of one input spent this way.
        pub satisfaction_wu: u64,
    }

    #[derive(Debug, Clone)]
    pub struct MusigSpendLeaves {
        pub is_ok: bool,
        pub error: String,
        /// Key path first, then leaves in DFS order.
        pub leaves: Vec<MusigSpendLeaf>,
    }

    /// First receive address of a musig() descriptor, before any account holds it,
    /// with what IKeys::verifyAddress needs (the AddressVerifyInfo of that address).
    #[derive(Debug, Clone)]
    pub struct MusigReceiveAddress {
        pub is_ok: bool,
        pub error: String,
        pub address: String,
        pub descriptor: String,
        pub path: String,
    }

    // ===== Policy =====

    /// Role of a key (bwk-bup `KeyType`), stored in its origin's last step R.
    #[derive(Debug, Clone, Copy)]
    pub enum PolicyKeyType {
        Internal,
        External,
        Cosigner,
        SafetyNet,
        /// Review only: the origin carries no known R.
        Unknown,
    }

    /// Shape of a path's keys (bwk-bup `Semantic`).
    #[derive(Debug, Clone, Copy)]
    pub enum PolicySemantic {
        Single,
        /// m-of-n; every multisig compiles to MuSig2 leaves.
        Multi,
        /// Mandatory keys plus a subset of the others.
        MultiMandatory,
        /// Review only: nested, custom or unknown shapes.
        Other,
    }

    /// Timelock kind of a path (bwk-bup `Locktime`).
    #[derive(Debug, Clone, Copy)]
    pub enum PolicyLocktime {
        None,
        Relative,
        AbsoluteRenewable,
        Absolute,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum PolicyDurationUnit {
        Blocks,
        Days,
        Weeks,
        Months,
    }

    #[derive(Debug, Clone, Copy)]
    pub enum PolicyDraftError {
        None,
        Count,
        Date,
        Future,
        Tip,
        RelativeMax,
        Bounds,
    }

    #[derive(Debug, Clone)]
    pub struct PolicyDraft {
        pub keys: Vec<String>,
        pub mandatory: Vec<String>,
        pub threshold: u32,
        pub locktime: PolicyLocktime,
        pub count: u32,
        pub unit: PolicyDurationUnit,
        pub date_timestamp: i64,
        pub date_valid: bool,
    }

    #[derive(Debug, Clone)]
    pub struct PolicyDraftResult {
        pub is_ok: bool,
        pub error: String,
        pub lock_error: PolicyDraftError,
        pub path: PolicyPath,
    }

    /// One key of a path, as the creator gathered it. The role key
    /// `[fp/87'/coin'/account'/R]xpub` is its role xpub, fetched at R 0 (Internal).
    #[derive(Debug, Clone)]
    pub struct PolicyKey {
        pub fingerprint: String,
        /// The BIP-87 account path `m/87'/coin'/account'`.
        pub account_path: String,
        /// The xpub at `account_path`/0, the Internal role's; ignored by preview_policy.
        pub role_xpub: String,
        pub key_type: PolicyKeyType,
        /// MultiMandatory only: the key signs every leaf.
        pub mandatory: bool,
    }

    #[derive(Debug, Clone)]
    pub struct PolicyPath {
        pub semantic: PolicySemantic,
        /// Signers per spend: 1 for Single, m for Multi, mandatory plus cosigners
        /// for MultiMandatory.
        pub threshold: u32,
        pub locktime: PolicyLocktime,
        /// Blocks for Relative, height for the absolute kinds, 0 for None.
        pub locktime_value: u32,
        pub keys: Vec<PolicyKey>,
    }

    /// A policy from the creator. The binding infers the bwk-bup `PolicyType` from
    /// the paths: PrimaryOnly (one locktime-free path, no recovery), Csv, Cltv,
    /// CsvWithMandatoryKey, Unknown (an absolute timelock beside mandatory keys) or
    /// Invalid (mixed timelock kinds, or several paths and none timelocked).
    #[derive(Debug, Clone)]
    pub struct PolicySpec {
        pub network: Network,
        pub paths: Vec<PolicyPath>,
    }

    #[derive(Debug, Clone)]
    pub struct PolicyCompileResult {
        pub is_ok: bool,
        pub error: String,
        /// Index in PolicySpec.paths the error is about, -1 for the whole policy.
        pub error_path: i32,
        /// The error is PolicyError::TooManyLeaves.
        pub too_many_leaves: bool,
        /// Standard BIP-390 text with checksum; empty on error and from preview_policy.
        pub descriptor: String,
        /// Tap leaves, padding excluded.
        pub leaf_count: u32,
        /// bwk-bup MAX_LEAVES.
        pub max_leaves: u32,
        /// The primary path is the internal key (key path spend).
        pub has_key_path: bool,
    }

    #[derive(Debug, Clone)]
    pub struct PolicyReviewKey {
        pub fingerprint: String,
        /// Origin path, `m/87'/coin'/account'/R` form.
        pub origin_path: String,
        pub xpub: String,
        pub key_type: PolicyKeyType,
        pub mandatory: bool,
    }

    #[derive(Debug, Clone)]
    pub struct PolicyReviewPath {
        pub semantic: PolicySemantic,
        pub threshold: u32,
        pub locktime: PolicyLocktime,
        pub locktime_value: u32,
        pub keys: Vec<PolicyReviewKey>,
        /// Its tap leaves; 0 on the key path.
        pub leaf_count: u32,
        pub key_path: bool,
    }

    /// A descriptor lifted back to paths and key types, the C++ side writes the
    /// plain words from it.
    #[derive(Debug, Clone)]
    pub struct PolicyReview {
        pub is_ok: bool,
        pub error: String,
        pub paths: Vec<PolicyReviewPath>,
        pub leaf_count: u32,
    }

    extern "Rust" {
        fn coordination_prepare(
            network: Network,
            psbt: &str,
            inputs: Vec<RustSelectedInput>,
            stage: SigningStage,
        ) -> CoordinationResult;
        fn coordination_accept(
            original: &str,
            returned: &str,
            inputs: Vec<RustSelectedInput>,
            stage: SigningStage,
        ) -> CoordinationResult;
        fn coordination_release(
            network: Network,
            psbt: &str,
            inputs: Vec<RustSelectedInput>,
            session_id: &str,
        ) -> CoordinationResult;
        fn transaction_finalize(
            network: Network,
            psbt: &str,
            inputs: Vec<RustSelectedInput>,
            session_id: &str,
        ) -> CoordinationResult;
        fn transaction_broadcast(
            network: Network,
            endpoint: &str,
            tx_hex: &str,
        ) -> CoordinationResult;
        /// `m/87'/coin'/account'` for `network`.
        fn bip87_account_path(network: Network, account: u32) -> String;
        /// `target` rounded up to the grid a renewable absolute timelock sits on.
        fn renewable_height(target: u32) -> u32;
        /// Compiles with placeholder keys (role_xpub ignored): leaf count, cap and
        /// shape errors before any device is asked. descriptor stays empty.
        fn preview_policy(spec: PolicySpec) -> PolicyCompileResult;
        /// Takes each key's role xpub as its role key, builds the bwk-bup Policy and
        /// compiles it, every multisig to MuSig2.
        fn compile_policy(spec: PolicySpec) -> PolicyCompileResult;
        /// `Policy::from_descriptor` of BIP-390 text, key types read from R.
        fn review_policy(descriptor: String) -> PolicyReview;
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

        /// A decoded and validated QR response.
        type QrResponse;

        /// GUI-thread copy of one key's record and the manager's network, for the
        /// free hot-key worker functions to sign/reveal/derive from off-thread.
        type HotMaterial;

        /// Worker results of the cached MuSig QR exchanges, built off-thread and
        /// turned into wire requests on the GUI thread.
        type QrMusigPrepare;
        type QrMusigSign;
        type FeedState;
        type EndpointRetry;
        fn new_endpoint_retry() -> Box<EndpointRetry>;
        fn retry_connected(self: &mut EndpointRetry);
        fn retry_stopped(self: &mut EndpointRetry);
        fn retry_lost(self: &mut EndpointRetry, now_ms: i64) -> RetryDecision;
        fn retry_due(self: &mut EndpointRetry) -> bool;
        type SpEndpointChange;
        type MusigEndpointChange;
        fn sp_startup_endpoint_change(account: Box<Account>) -> Box<SpEndpointChange>;
        fn sp_endpoint_change(
            account: Box<Account>,
            server: BackendServer,
            connected: bool,
        ) -> Box<SpEndpointChange>;
        fn musig_endpoint_change(
            account: Box<MusigAccount>,
            connected: bool,
        ) -> Box<MusigEndpointChange>;
        fn endpoint_phase(self: &SpEndpointChange) -> EndpointPhase;
        fn endpoint_server(self: &SpEndpointChange) -> BackendServer;
        fn endpoint_result(self: &SpEndpointChange) -> EndpointResult;
        fn prepare_endpoint(self: &mut SpEndpointChange);
        fn finish_endpoint_after_drain(self: &mut SpEndpointChange);
        fn endpoint_phase(self: &MusigEndpointChange) -> EndpointPhase;
        fn endpoint_server(self: &MusigEndpointChange) -> BackendServer;
        fn endpoint_result(self: &MusigEndpointChange) -> EndpointResult;
        fn prepare_endpoint(self: &mut MusigEndpointChange);
        fn finish_endpoint_after_drain(self: &mut MusigEndpointChange);
        fn take_sp_endpoint_account(change: Box<SpEndpointChange>) -> Box<Account>;
        fn take_musig_endpoint_account(change: Box<MusigEndpointChange>) -> Box<MusigAccount>;
        fn new_feed_state() -> Box<FeedState>;
        fn add_feed(self: &mut FeedState, id: &str);
        fn remove_feed(self: &mut FeedState, id: &str);
        fn select_price(self: &mut FeedState, source: String, selected_currency: String);
        fn set_currencies(self: &mut FeedState, id: &str, offered: Vec<String>);
        fn set_price_quote(self: &mut FeedState, id: &str, code: &str, value: f64);
        fn feed_source(self: &FeedState, id: &str) -> FeedSourceState;
        fn price_selection(self: &FeedState) -> FeedPrice;
        fn select_fees(self: &mut FeedState, preferences: RustFeePreferences) -> bool;
        fn set_fee_capabilities(self: &mut FeedState, id: &str, has_default: bool, has_smart: bool);
        fn set_default_fee(self: &mut FeedState, id: &str, value: f64);
        fn set_smart_fees(self: &mut FeedState, id: &str, low: f64, medium: f64, fast: f64);
        fn fee_selection(self: &FeedState) -> FeedSelection;
        type KeyFilePreview;
        type KeyFileCommit;

        /// A mnemonic sealed by `hot_seal`, committed by `KeysModule::commit_hot_key`.
        type SealedHotKey;

        /// A running camera, opened by `camera_open`. Dropping it stops the capture.
        type CameraHandle;

        /// The BBQR frames of one payload.
        type BbqrFrames;

        /// Joins BBQR parts scanned off camera frames back into payloads.
        type BbqrScanner;

        /// A MuSig2 policy account, wrapping bwk::Account.
        type MusigAccount;
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

        /// Render `text` (an address) as a single QR code.
        fn qr_text_image(text: &str) -> QrImageResult;

        /// Splits `payload` into BBQR parts of `part_bytes`, one QR image each. Touches
        /// nothing shared: safe on a worker thread.
        fn bbqr_frames(payload: &[u8], part_bytes: u32) -> Box<BbqrFrames>;
        fn is_ok(self: &BbqrFrames) -> bool;
        fn get_error(self: &BbqrFrames) -> String;
        fn count(self: &BbqrFrames) -> u32;
        /// An index past the end answers with is_ok false.
        fn frame(self: &BbqrFrames, index: u32) -> QrImageResult;

        fn bbqr_scanner_new() -> Box<BbqrScanner>;
        /// Also reads plain codes: each one's text is a payload, as an address code.
        fn bbqr_text_scanner_new() -> Box<BbqrScanner>;
        fn is_ok(self: &BbqrScanner) -> bool;
        fn get_error(self: &BbqrScanner) -> String;
        /// Scans one RGB888 frame of `width * height * 3` bytes.
        fn scan(self: &mut BbqrScanner, rgb: &[u8], width: u32, height: u32) -> BbqrScanResult;
        /// Drops the parts seen so far.
        fn reset(self: &mut BbqrScanner);

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
        fn test_electrum(address: String, verify_certificate: bool) -> ConnectionResult;
        /// Test, then persist the complete endpoint. Does not start a wallet listener.
        fn test_and_save_endpoint(
            account_name: String,
            server: BackendServer,
            url: String,
            verify_certificate: bool,
        ) -> EndpointResult;
        /// Tests the endpoint against `network`, saving nothing. Blocking.
        fn test_endpoint(
            server: BackendServer,
            url: String,
            verify_certificate: bool,
            network: Network,
        ) -> EndpointResult;
        /// Persists `endpoints` to the account's config, untested, all or none. Does
        /// not touch a live connection.
        fn save_endpoints(account_name: String, endpoints: Vec<EndpointSetting>) -> EndpointResult;

        /// The height of the chain tip the Electrum server at `address` follows.
        /// Blocking: call it off the GUI thread.
        fn electrum_tip_height(address: String, verify_certificate: bool) -> ElectrumTipResult;

        /// Fetch default regtest infrastructure addresses from minta API.
        /// Blocking HTTP call - should be called from a background thread.
        fn get_regtest_defaults() -> RegtestDefaults;

        /// List enabled app-level plugin IDs.
        fn app_enabled_plugins() -> Vec<String>;
        fn app_begin_plugin_removal(id: String) -> String;
        fn app_disable_plugin(id: String) -> String;
        fn app_removing_plugins() -> PluginRemovalListResult;

        /// Enable or disable a plugin ID at app level.
        fn app_set_plugin_enabled(id: String, enabled: bool) -> String;

        /// Get the active app theme name.
        fn app_active_theme() -> String;

        /// Set the active app theme name.
        fn app_set_active_theme(name: String);
        fn app_fee_preferences() -> RustFeePreferences;
        fn app_set_fee_preferences(preferences: RustFeePreferences) -> String;
        fn app_price_source() -> String;
        fn app_price_currency() -> String;
        fn app_set_price_selection(source: String, currency: String);

        /// Get the app language (a locale such as "fr"), empty until one is picked.
        fn app_language() -> String;

        /// Set the app language.
        fn app_set_language(locale: String);

        /// The QR display tuning picked last, the middle level until one is.
        fn app_qr_display_levels() -> QrDisplayLevels;

        /// Remember the QR display tuning for every account.
        fn app_set_qr_display_levels(levels: QrDisplayLevels);

        /// The camera contrast level picked last, the middle level until one is.
        fn app_qr_camera_contrast_level() -> u8;

        /// Remember the camera contrast level for every account.
        fn app_set_qr_camera_contrast_level(level: u8);

        /// The `server` address last used to create an account on `network`, empty
        /// until one is.
        fn app_last_endpoint(server: BackendServer, network: Network) -> String;

        /// Remember the `server` address an account on `network` was created with.
        fn app_set_last_endpoint(server: BackendServer, network: Network, url: String);

        /// The app state's unlock key, before anything else reads the state.
        fn state_unlock_info() -> StateUnlockInfo;

        /// A state file found in clear, empty when there is none: the app stops.
        fn state_clear_file() -> String;

        /// Forgets the key: the state cannot be read or written until the next unlock.
        fn state_lock();

        /// A hardened index for a new device key, drawn from the system random source.
        /// A failed draw is an error: never a fixed index.
        fn state_draw_index() -> StateIndexResult;

        /// The path a device unlock key is fetched at, "m/138'/1936225652'/<index>'".
        fn state_device_path(index: u32) -> String;

        /// Creates the state, unlocked by the device whose xpub at the device path of
        /// `index` is `xpub`. Answers the error, empty on success.
        fn state_create_with_device(index: u32, xpub: String) -> String;

        /// Creates the state, unlocked by `password`. Blocks on Argon2id: off the GUI
        /// thread. Answers the error, empty on success.
        fn state_create_with_password(password: String) -> String;

        /// Unlocks with the device xpub. Answers the error, empty on success.
        fn state_unlock_with_device(xpub: String) -> String;

        /// Unlocks with `password`. Blocks on Argon2id: off the GUI thread. Answers the
        /// error, empty on success.
        fn state_unlock_with_password(password: String) -> String;

        /// Seals the plugin bundle dropped at `path`, whatever its file name, into the
        /// state once its library is admitted for the host, as `plugin_admission`,
        /// replacing an earlier version of it.
        fn plugin_install(
            path: String,
            expected_iid: String,
            qt_major: u8,
            qt_minor: u8,
            arch_requirements: u8,
        ) -> PluginInstallResult;

        fn plugin_admission(
            bytes: &[u8],
            expected_iid: String,
            qt_major: u8,
            qt_minor: u8,
            arch_requirements: u8,
        ) -> String;

        /// Every installed plugin's sidecar, as JSON text.
        fn plugin_bundles() -> Vec<String>;
        fn plugin_metadata() -> Vec<PluginMetadata>;

        /// The library and catalogs of the installed plugin `plugin_id`.
        fn plugin_library(plugin_id: String) -> PluginLibraryResult;

        /// Host only, after a durable removal checkpoint and completed native unload.
        /// Purges private/legacy data, bundle and grants. Empty error means completion.
        fn plugin_remove(plugin_id: String) -> String;

        /// Opens a storage session for the plugin the host authenticated as
        /// `plugin_id`, its library `plugin_hash`. Host only: the caller is never asked
        /// who it is. `external` false for a module the host ships, which needs no
        /// Saved Data grant. Answers 0 when refused.
        fn storage_session_open(plugin_id: String, plugin_hash: String, external: bool) -> u64;

        /// Ends the session on unload: outstanding requests answer `revoked`. Never
        /// waits for a request.
        fn storage_session_revoke(session: u64);

        /// Answers the JSON storage request on `session` with a JSON response. Blocks
        /// on disk and on other sessions' requests: off the GUI thread.
        fn storage_request(session: u64, request: String) -> String;

        /// Cancels the request `id` on `session`, sent or not yet: it answers
        /// `cancelled`. A request already replacing its tree may complete.
        fn storage_request_cancel(session: u64, id: String);

        /// Forgets the cancellation of `id` on `session`, once its `storage_request`
        /// returned or will never be made.
        fn storage_request_forget(session: u64, id: String);

        /// The session's generation now, taken when a request is sent and given back
        /// to `storage_delivery_error` with its answer. 0 for a closed session.
        fn storage_session_generation(session: u64) -> u64;

        /// Why an answer computed at `generation` must not be delivered: "revoked",
        /// "locked", "denied" once the grant or the key changed, empty when it may.
        /// Reads memory only: safe on the GUI thread.
        fn storage_delivery_error(session: u64, generation: u64) -> String;

        /// The answers given to external plugin libraries, in the order they were given.
        fn app_plugin_access() -> Vec<PluginAccess>;
        fn app_plugin_answers(plugin_id: String, hash: String) -> PluginAccess;
        fn app_plugin_grants(hash: String) -> PluginGrantedAccess;
        fn app_plugin_history(plugin_id: String) -> Vec<PluginAccess>;
        fn app_plugin_access_request(plugin_id: String, hash: String) -> PluginAccessRequest;
        /// Reads an atomic only; odd while a grant publication is in progress.
        fn app_plugin_access_revision() -> u64;
        fn app_plugin_access_history(plugin_id: String) -> PluginAccessHistory;

        /// Stores the answers for `access.hash`, replacing earlier ones. An entry left
        /// with both grants unasked is dropped, so the host asks again. Refused, with
        /// the error, while the plugin's removal is pending or when its storage epoch
        /// is no longer `expected_epoch`, the one its request or history was read with.
        fn app_set_plugin_access(access: PluginAccess, expected_epoch: u64) -> String;
        /// Also refuses a stale persisted grant version, checked under both state locks.
        fn app_set_plugin_access_if_current(
            access: PluginAccess,
            expected_epoch: u64,
            expected_version: u64,
        ) -> String;

        /// `psbt`, a serialized PSBTv2, as base64 text with its txid when it has one.
        fn export_psbt(psbt: &[u8]) -> PsbtExport;

        /// Writes the PSBT to `path` exactly as serialized. Refuses to overwrite.
        fn sd_save_psbt(path: &str, psbt_hex: &str) -> PsbtFileResult;
        fn sd_load_psbt(path: &str) -> PsbtFileResult;

        /// Writes the BIP138 backup of the musig() `descriptor` to `path`, replacing
        /// any file there. `store_common_paths` false is the privacy mode: the common
        /// derivation paths stay out of the clear part.
        fn save_descriptor_backup(
            descriptor: &str,
            path: &str,
            store_common_paths: bool,
        ) -> DescriptorBackupResult;
    }

    // ===== Config Methods =====

    extern "Rust" {
        /// Create a new config.
        fn parse_btc_amount(text: &str) -> AmountParseResult;
        fn validation_error_kind(result: &PsbtValidation) -> BackendErrorKind;
        fn notification_error_kind(notification: &Notification) -> BackendErrorKind;
        fn parse_fee_input(text: &str, rate: bool) -> FeeParseResult;
        fn normalize_fee_rate(rate: f64) -> FeeParseResult;
        fn send_amounts(inputs: &[u64], recipients: &[u64], fee: u64) -> SendAmounts;
        fn policy_threshold(picked: u32, mandatory: u32, threshold: u32) -> u32;
        fn policy_duration_blocks(count: u32, unit: PolicyDurationUnit) -> u64;
        fn resolve_policy_draft(
            draft: PolicyDraft,
            network: Network,
            has_tip: bool,
            tip: u32,
            now: i64,
        ) -> PolicyDraftResult;
        fn create_sp_account(request: SpCreationRequest) -> AccountCreationResult;
        fn create_musig_account(request: MusigCreationRequest) -> AccountCreationResult;

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
        fn list_module_configs(plugin_id: &str) -> Vec<String>;

        /// Delete an account's data from disk.
        fn sp_account_delete_check(name: String) -> String;
        fn musig_account_delete_check(name: String) -> String;
        fn delete_sp_account(name: String) -> String;
        fn delete_musig_account(name: String) -> String;

        /// The segwit sub-account descriptor of a key, from its xpub at 84'/coin'/0'.
        /// Empty when the fingerprint or the xpub is invalid.
        fn segwit_descriptor_for(fingerprint: String, xpub: String, network: Network) -> String;

        /// The taproot sub-account descriptor of a key, from its xpub at 86'/coin'/0'.
        /// Empty when the fingerprint or the xpub is invalid.
        fn taproot_descriptor_for(fingerprint: String, xpub: String, network: Network) -> String;

        /// Check if the config was loaded successfully.
        fn is_ok(self: &Config) -> bool;

        /// Get error message (empty if is_ok).
        fn get_error(self: &Config) -> String;

        /// Save config to file.
        fn to_file(self: &Config);
        /// Empty on success, otherwise the write error.
        fn save(self: &Config) -> String;

        /// Get SP descriptor.
        fn get_descriptor(self: &Config) -> String;

        /// Set SP descriptor.
        fn set_descriptor(self: &mut Config, descriptor: String);

        /// The segwit sub-account descriptor, empty for none.
        fn get_segwit_descriptor(self: &Config) -> String;

        /// Set the segwit sub-account descriptor, empty for none.
        fn set_segwit_descriptor(self: &mut Config, descriptor: String);

        /// The taproot sub-account descriptor, empty for none.
        fn get_taproot_descriptor(self: &Config) -> String;

        /// Set the taproot sub-account descriptor, empty for none.
        fn set_taproot_descriptor(self: &mut Config, descriptor: String);

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

        /// Whether the Electrum server's TLS certificate is checked.
        fn get_verify_certificate(self: &Config) -> bool;

        /// Check the Electrum server's TLS certificate, or not (a self-signed server).
        fn set_verify_certificate(self: &mut Config, verify: bool);

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
        fn new_account_offline(config: &Config) -> Box<Account>;

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

        /// The account's network (Regtest when it failed to load).
        fn network(self: &Account) -> Network;
        fn electrum_url(self: &Account) -> String;
        fn blindbit_url(self: &Account) -> String;
        fn verify_certificate(self: &Account) -> bool;
        fn electrum_running(self: &Account) -> bool;

        /// Whether the account has a segwit (wpkh) sub-account.
        fn has_segwit(self: &Account) -> bool;

        /// Whether the account has a taproot (tr) sub-account.
        fn has_taproot(self: &Account) -> bool;

        /// Generate a new segwit (wpkh) receiving address.
        fn new_segwit_addr(self: &mut Account) -> String;

        /// Generate a new taproot receiving address.
        fn new_taproot_addr(self: &mut Account) -> String;

        /// Get all coins (spent and unspent).
        fn coins(self: &Account) -> Vec<RustCoin>;
        fn spending_coins(self: &Account) -> SpendCoinsResult;

        /// Get spendable coins summary.
        fn spendable_coins(self: &Account) -> CoinState;

        /// Update a coin's label.
        fn update_coin_label(self: &Account, outpoint: String, label: String) -> LabelEditResult;

        /// Get payment history.
        fn payment_history(self: &Account) -> Vec<RustTx>;
        fn reopen_backend(self: &mut Account, connect_electrum: bool) -> EndpointResult;
        fn close_backend(self: &mut Account);
        fn shutdown_backend(self: &mut Account) -> String;
        fn tx_details(self: &Account, txid: &str, allow_network: bool) -> RustTxDetails;
        fn update_tx_label(self: &Account, txid: String, label: String) -> LabelEditResult;

        /// Get the account SP descriptor.
        fn descriptor(self: &Account) -> String;

        /// Descriptor and derivation path to verify `address` on a signing device.
        fn address_verify_info(self: &Account, address: &str) -> AddressVerifyInfo;

        /// Simulate a transaction to check feasibility and estimate fees.
        /// Returns simulation result with fee, weight, and validity info.
        fn simulate_transaction(
            self: &Account,
            tx_template: TransactionTemplate,
        ) -> TransactionSimulation;

        /// Finalize a signed PSBT and return transaction hex.
        fn finalize_psbt(self: &Account, signed_psbt_base64: String) -> TxResult;

        /// Log all details of a failed broadcast for debugging/reproduction.
        /// Called after a broadcast is not accepted.
        fn log_failed_broadcast(
            self: &Account,
            tx_template: TransactionTemplate,
            signed_tx_hex: String,
        );

        /// Validates an unsigned PSBTv2, a finalize_transaction answer, via Electrum before
        /// signing: output address reuse and already-spent inputs. Blocking.
        fn validate_unsigned_psbt(self: &Account, psbt: &[u8]) -> PsbtValidation;
        /// The PSBT `text` (base64, or binary) holds, spending only the account's unspent
        /// coins, with the transaction it states. Pure: changes no account state.
        fn import_psbt(self: &Account, text: &[u8]) -> PsbtImport;

        /// Coins, balance and history for the published account read model.
        fn funds_snapshot(self: &Account) -> FundsSnapshot;
        /// The root and every sub-account label, RAM edits included.
        fn labels(self: &Account) -> LabelSnapshot;
        /// An empty text removes the label.
        fn set_label(self: &Account, kind: LabelKind, id: String, text: String) -> LabelEditResult;
        fn receive_kinds(self: &Account) -> Vec<ReceiveKind>;
        /// The silent payment address, then every generated sub-account address.
        fn receive_addresses(self: &Account) -> Vec<ReceiveAddress>;
        /// Hands out an address of `kind`: the static one for "sp". Persists the tip.
        fn new_receive_address(self: &Account, kind: &str) -> ReceiveAddressResult;
        /// Checks the template against its own inputs: none given is incomplete.
        fn craft_transaction(self: &Account, tx_template: TransactionTemplate) -> CraftOutcome;
        /// Replaces the template's inputs by the ones coin selection picks.
        fn select_transaction_coins(
            self: &Account,
            tx_template: TransactionTemplate,
        ) -> CraftOutcome;
        /// The unsigned PSBTv2 of a template with inputs. Changes no account state.
        fn finalize_transaction(self: &Account, tx_template: TransactionTemplate) -> UnsignedPsbt;
        /// Broadcasts consensus-encoded `transaction`, then records it locally.
        fn broadcast_report(self: &Account, transaction: &[u8]) -> BroadcastReport;
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
        /// Like get_xpub, shown on the device: a Ledger gives a non-standard path only so.
        fn get_xpub_shown(self: &HwiService, req_id: u64, device_id: &str, path: &str) -> bool;
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
        /// Signs for the registered wallet of `descriptor`. `hmac` is the stored proof
        /// of the descriptor on this key, hex, empty when none.
        fn sign_psbt(
            self: &HwiService,
            req_id: u64,
            device_id: &str,
            psbt_base64: &str,
            descriptor: &str,
            hmac: &str,
        ) -> bool;

        /// Asks the device to show the address at `path`. A single key BIP-86 path is
        /// shown directly, any other descriptor is shown as the registered wallet's
        /// address at `change/index`. `hmac` is the stored proof of the descriptor on
        /// this key, hex, empty when none.
        fn display_address(
            self: &HwiService,
            req_id: u64,
            device_id: &str,
            descriptor: &str,
            path: &str,
            hmac: &str,
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
        /// Whether a device's capability bits include silent payments.
        fn supports_silent_payments(capabilities: u32) -> bool;
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
        fn check_device_signing(
            self: &KeysModule,
            psbt_v2_hex: &str,
            fingerprint: &str,
            capabilities: u32,
        ) -> KeyOpResult;
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
        /// Drops a cached xpub that is not used and that no registered descriptor
        /// carries.
        fn remove_xpub(self: &mut KeysModule, fingerprint: &str, path: &str) -> KeyOpResult;
        fn key_xpubs(self: &KeysModule, fingerprint: &str) -> Vec<KeyXpub>;
        /// Applies usage metadata already persisted by the account discovery worker.
        fn apply_xpub_usage(self: &mut KeysModule, usage: &AccountXpubUsage) -> bool;
        fn register_descriptor(
            self: &mut KeysModule,
            fingerprint: &str,
            descriptor: &str,
        ) -> KeyOpResult;
        /// Records `descriptor` as registered with the key, with the proof the device
        /// returned (`hmac`, 32 bytes of hex).
        fn set_descriptor_hmac(
            self: &mut KeysModule,
            fingerprint: &str,
            descriptor: &str,
            hmac: &str,
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
        /// Whether `word` is in the English BIP39 word list.
        fn bip39_word_is_valid(word: &str) -> bool;
        /// Whether `mnemonic` is a BIP39 mnemonic: known words, a supported length
        /// and a matching checksum. Any length BIP39 allows, so 15 words too.
        fn mnemonic_is_valid(mnemonic: &str) -> bool;

        /// Copies out the record and the manager's network for the free hot-key
        /// worker functions below, so a `set_network` or route change afterward
        /// cannot race a worker already in flight against the copy. Fails for an
        /// unknown key or a key without a secret.
        fn hot_material(self: &KeysModule, fingerprint: &str) -> Box<HotMaterial>;
        fn key_file_preview(path: &str, network: Network) -> Box<KeyFilePreview>;
        fn is_ok(self: &KeyFilePreview) -> bool;
        fn get_error(self: &KeyFilePreview) -> String;
        fn file_failure(self: &KeyFilePreview) -> KeyFileFailure;
        fn file_network(self: &KeyFilePreview) -> Network;
        fn file_fingerprint(self: &KeyFilePreview) -> String;
        fn file_alias(self: &KeyFilePreview) -> String;
        fn file_xpubs(self: &KeyFilePreview) -> Vec<KeyXpub>;
        fn commit_key_file(preview: &KeyFilePreview, alias: &str) -> Box<KeyFileCommit>;
        fn is_ok(self: &KeyFileCommit) -> bool;
        fn get_error(self: &KeyFileCommit) -> String;
        fn get_error_kind(self: &KeyFileCommit) -> BackendErrorKind;
        fn apply_key_file(self: &mut KeysModule, committed: &KeyFileCommit) -> KeyOpResult;
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
        fn hot_sp_descriptor(material: &HotMaterial, password: &str) -> HotSpDescriptorResult;

        /// The xpub at `path` of `material`'s hot secret, for an account's segwit or
        /// taproot sub-account. Blocks on Argon2id while opening the envelope: call
        /// from a QThread, never from the GUI thread.
        fn hot_xpub(material: &HotMaterial, password: &str, path: &str) -> HotXpubResult;

        /// Converts a v2 PSBT into its v0 base64 payload for a USB signing device.
        /// Cheap; safe to call from the GUI thread. Refuses a PSBT with a pending SP
        /// output: a signing device only ever signs its own bip32 inputs, and the
        /// signing plan already blocks every other candidate while one is pending. In
        /// MuSig2 round 1 every nonce and partial signature is dropped, as the QR route
        /// does, because a device takes any nonce in the PSBT as round 2;
        /// `usb_merge_signed` merges into the unstripped original, so the nonces
        /// already gathered stay. With `open_musig_session` (a Coldcard) and a wallet
        /// `descriptor` whose internal key is a musig(), an input listing no MuSig2
        /// participants gets the internal key's: the Coldcard needs a MuSig2 session
        /// for such a wallet even on a single-key leaf. The nonce it may add for that
        /// key path is dropped by `usb_merge_signed`.
        fn usb_sign_payload(
            psbt_v2_hex: &str,
            descriptor: &str,
            open_musig_session: bool,
        ) -> PsbtPayloadResult;

        /// Merges a USB device's v0 signed answer back into the original v2 PSBT,
        /// keeping its silent-payment input tweaks and ECDH shares. Only signature
        /// fields are copied. Cheap; safe to call from the GUI thread.
        fn usb_merge_signed(original_v2_hex: &str, signed_v0_base64: &str) -> SignResult;

        type LedgerNonceFetch;
        fn hot_create_sessions(
            material: &HotMaterial,
            password: &str,
            policy: &str,
            quantity: u32,
        ) -> MusigSessionCreateResult;
        fn hot_cached_nonces(material: &HotMaterial, psbt_v2_hex: &str) -> HotCachedNonceResult;
        /// Persists xpub usage from the accounts of `network`, returning cache updates.
        /// Reads and writes storage: call it off the GUI thread.
        type AccountXpubUsage;
        fn is_ok(self: &AccountXpubUsage) -> bool;
        fn get_error(self: &AccountXpubUsage) -> String;
        fn account_xpub_usage(network: Network) -> Box<AccountXpubUsage>;
        fn musig_session_refresh(fingerprint: &str) -> KeyOpResult;
        fn musig_session_count(fingerprint: &str, policy: &str, provider: KeyRouteKind) -> u32;
        fn musig_session_limit() -> u32;
        fn musig_session_plan(
            network: Network,
            psbt_v2_hex: &str,
            devices: Vec<LiveDevice>,
        ) -> SigningPlan;
        fn musig_session_restart(
            network: Network,
            policy: &str,
            original: &str,
            prepared: &str,
        ) -> SignResult;

        // Disk operations: callers run these on a worker, never the GUI thread.
        fn qr_musig_prepare_material(fingerprint: &str, descriptor: &str) -> Box<QrMusigPrepare>;
        fn qr_musig_sign_material(
            fingerprint: &str,
            policy: &str,
            psbt_v2_hex: &str,
            routes: Vec<QrMusigSignerRoute>,
        ) -> Box<QrMusigSign>;
        fn qr_musig_accept(
            answer: Box<QrResponse>,
            fingerprint: &str,
            policy: &str,
            psbt_v2_hex: &str,
        ) -> QrMusigCacheResult;
        // CPU only: builds the wire request and makes it the outstanding one.
        fn qr_request_prepare_musig(
            self: &mut KeysModule,
            preparation: Box<QrMusigPrepare>,
        ) -> QrMessageResult;
        fn qr_request_sign_musig(
            self: &mut KeysModule,
            signing: Box<QrMusigSign>,
        ) -> QrMessageResult;
        // Disk operations: callers run these on a worker, never the GUI thread.
        fn ledger_nonce_fetch(
            fingerprint: &str,
            policy: &str,
            quantity: u32,
        ) -> Box<LedgerNonceFetch>;
        fn ledger_nonce_fetch_for_signing(
            fingerprint: &str,
            policy: &str,
            psbt_v2_hex: &str,
        ) -> Box<LedgerNonceFetch>;
        fn payload(self: &LedgerNonceFetch) -> PsbtPayloadResult;
        fn accept(self: &mut LedgerNonceFetch, signed_psbt: &str) -> PsbtPayloadResult;
        fn ledger_nonce_refresh(fingerprint: &str) -> KeyOpResult;
        fn ledger_nonce_plan(
            network: Network,
            psbt_v2_hex: &str,
            devices: Vec<LiveDevice>,
        ) -> SigningPlan;
        fn ledger_nonce_invalidate(
            fingerprint: &str,
            policy: &str,
            psbt_v2_hex: &str,
        ) -> SignResult;
        fn ledger_nonce_restart(
            network: Network,
            policy: &str,
            original: &str,
            prepared: &str,
        ) -> SignResult;
        // Memory-only snapshot for rows and offline route selection.
        fn ledger_nonce_count(fingerprint: &str, policy: &str) -> u32;
        fn ledger_nonce_limit() -> u32;
        fn ledger_nonce_merge(
            fingerprint: &str,
            policy: &str,
            original: &str,
            signed_psbt: &str,
        ) -> SignResult;
        fn ledger_nonce_sign(
            fingerprint: &str,
            policy: &str,
            psbt_v2_hex: &str,
        ) -> LedgerNonceSignResult;

        /// Merges a signer's v2 answer, made on an earlier copy of the PSBT, into
        /// `current_v2_hex`: its signatures and MuSig2 nonces / partial signatures join
        /// the ones gathered since, so several signers can work at once. An answer
        /// adding nothing new leaves `current_v2_hex` as it is. Cheap; GUI thread.
        fn merge_signed_psbt(current_v2_hex: &str, signed_v2_hex: &str) -> SignResult;
    }

    // ===== Keys QR Methods =====

    extern "Rust" {
        // Each builder makes its request the outstanding one a response is checked
        // against.
        fn qr_request_get_xpubs(self: &mut KeysModule, paths: Vec<String>) -> QrMessageResult;
        fn qr_request_register_descriptor(
            self: &mut KeysModule,
            descriptor: &str,
        ) -> QrMessageResult;
        fn qr_request_verify_address(
            self: &mut KeysModule,
            descriptor: &str,
            path: &str,
            address: &str,
        ) -> QrMessageResult;
        fn qr_request_sign(
            self: &mut KeysModule,
            descriptors: Vec<String>,
            psbt_v2_hex: &str,
        ) -> QrMessageResult;
        fn qr_request_get_sp_account(self: &mut KeysModule) -> QrMessageResult;

        /// Decodes a response's bytes and validates them against the outstanding
        /// request.
        fn qr_decode_response(self: &mut KeysModule, payload: &[u8]) -> Box<QrResponse>;

        fn is_ok(self: &QrResponse) -> bool;
        fn get_error(self: &QrResponse) -> String;
        /// Only meaningful when !is_ok().
        fn error_kind(self: &QrResponse) -> QrErrorKind;
        /// Only meaningful when is_ok().
        fn kind(self: &QrResponse) -> QrResponseKind;
        /// The signer's fingerprint when kind() is Xpubs or SpAccount, else empty.
        fn fingerprint(self: &QrResponse) -> String;
        /// The SP descriptor when kind() is SpAccount, else empty.
        fn descriptor(self: &QrResponse) -> String;
        /// The signed PSBT as v2 hex when kind() is SignedPsbt, else empty.
        fn psbt_hex(self: &QrResponse) -> String;
        /// The device vendor when kind() is Xpubs, else empty.
        fn vendor(self: &QrResponse) -> String;
        /// The device model when kind() is Xpubs, else empty.
        fn model(self: &QrResponse) -> String;
        /// The device firmware version when kind() is Xpubs, else empty.
        fn version(self: &QrResponse) -> String;
        /// The device capability bits when kind() is Xpubs, else 0.
        fn capabilities(self: &QrResponse) -> u32;
        /// The derived xpubs when kind() is Xpubs, the taproot and segwit xpubs when
        /// SpAccount, else empty.
        fn xpubs(self: &QrResponse) -> Vec<KeyXpub>;
        /// The device's verify_address answer when kind() is AddressVerified, else false.
        fn verified(self: &QrResponse) -> bool;
        fn registered(self: &QrResponse) -> bool;
    }

    // ===== Camera Methods =====

    extern "Rust" {
        fn camera_list() -> CameraListResult;
        /// Spawns the capture thread for camera `index`; the device opens inside it,
        /// and a failure there shows up later through `failure()`. Call from a
        /// worker thread, never the GUI thread.
        fn camera_open(index: u32) -> Box<CameraHandle>;
        fn is_ok(self: &CameraHandle) -> bool;
        fn get_error(self: &CameraHandle) -> String;
        /// The latest frame, is_ok false until the first one is captured.
        fn frame(self: &CameraHandle) -> CameraFrameResult;
        /// Why the capture stopped on its own, empty while it runs.
        fn failure(self: &CameraHandle) -> String;
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
        fn saved_record_id(self: &PsbtResult) -> String;
        fn save_error(self: &PsbtResult) -> String;
        fn get_error_kind(self: &PsbtResult) -> BackendErrorKind;
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

    // ===== Musig Utility Functions =====

    extern "Rust" {
        /// Parses BIP-390 text and returns its canonical form.
        fn validate_musig_descriptor(descriptor: String) -> MusigDescriptorResult;
        /// Every cosigner fingerprint of a musig() descriptor, empty on error.
        fn musig_descriptor_fingerprints(descriptor: String) -> Vec<String>;
        /// The receive address a new account of a musig() descriptor hands out first.
        fn musig_first_receive_address(descriptor: String, network: Network)
            -> MusigReceiveAddress;
    }

    // ===== Musig Account Methods =====

    extern "Rust" {
        /// Loads the account `account_name` (config plugin id "musig").
        /// Check is_ok() before use.
        fn new_musig_account(account_name: String) -> Box<MusigAccount>;
        fn new_musig_account_offline(config: &Config) -> Box<MusigAccount>;
        fn is_ok(self: &MusigAccount) -> bool;
        fn get_error(self: &MusigAccount) -> String;
        fn name(self: &MusigAccount) -> String;
        /// Regtest when it failed to load.
        fn network(self: &MusigAccount) -> Network;
        /// Standard BIP-390 text.
        fn descriptor(self: &MusigAccount) -> String;
        fn cosigner_fingerprints(self: &MusigAccount) -> Vec<String>;

        /// Reloads the Electrum endpoint from the config and starts the listener.
        fn start_electrum(self: &mut MusigAccount) -> bool;
        fn stop_electrum(self: &mut MusigAccount);
        fn electrum_url(self: &MusigAccount) -> String;
        /// Whether the Electrum server's TLS certificate is checked.
        fn verify_certificate(self: &MusigAccount) -> bool;
        /// Saves the endpoint and stops the listener. Reopen explicitly to connect.
        fn set_electrum_url(self: &mut MusigAccount, url: String) -> MusigElectrumUrlResult;

        fn try_recv(self: &mut MusigAccount) -> Box<Poll>;
        fn take_receiver(self: &mut MusigAccount) -> Box<NotificationReceiver>;

        fn balance(self: &MusigAccount) -> u64;
        fn new_addr(self: &mut MusigAccount) -> String;
        fn coins(self: &MusigAccount) -> Vec<RustCoin>;
        fn spending_coins(self: &MusigAccount) -> SpendCoinsResult;
        fn spendable_coins(self: &MusigAccount) -> CoinState;
        fn update_coin_label(
            self: &MusigAccount,
            outpoint: String,
            label: String,
        ) -> LabelEditResult;
        fn payment_history(self: &MusigAccount) -> Vec<RustTx>;
        fn tx_details(self: &MusigAccount, txid: &str, allow_network: bool) -> RustTxDetails;
        fn update_tx_label(self: &MusigAccount, txid: String, label: String) -> LabelEditResult;
        fn reopen_backend(self: &mut MusigAccount, connect_electrum: bool) -> EndpointResult;
        fn close_backend(self: &mut MusigAccount);
        fn shutdown_backend(self: &mut MusigAccount) -> String;
        fn address_verify_info(self: &MusigAccount, address: &str) -> AddressVerifyInfo;

        fn simulate_transaction(
            self: &MusigAccount,
            tx_template: TransactionTemplate,
        ) -> TransactionSimulation;
        /// Every spend path for the coins `outpoints`, `spendable` at the current tip.
        fn spend_leaves(self: &MusigAccount, outpoints: Vec<String>) -> MusigSpendLeaves;
        /// Per cosigner nonce / partial sig counts of the PSBT.
        fn round_status(self: &MusigAccount, psbt_v2_hex: &str) -> MusigRoundStatus;
        /// Aggregates partial sigs and returns the finalized PSBT and transaction.
        fn finalize_psbt(self: &MusigAccount, psbt_v2_hex: &str) -> MusigFinalizeResult;
        /// Disk I/O: call all saved PSBT mutations on the account's worker thread.
        fn saved_psbts(self: &MusigAccount, include_confirmed: bool) -> SavedPsbtList;
        fn restore_saved_psbt(self: &MusigAccount, id: &str) -> SavedPsbtRestore;
        fn update_saved_psbt(self: &MusigAccount, id: &str, psbt_v2_hex: &str)
            -> SavedPsbtMutation;
        fn reset_saved_psbt(self: &MusigAccount, id: &str) -> SavedPsbtReset;
        fn label_saved_psbt(self: &MusigAccount, id: &str, label: String) -> SavedPsbtMutation;
        fn delete_saved_psbt(self: &MusigAccount, id: &str) -> SavedPsbtMutation;
        fn retry_saved_psbts(self: &MusigAccount) -> SavedPsbtMutation;
        /// Saves the PSBT `text` (base64, or binary) holds once it spends the account's
        /// coins on one of its spend paths. A PSBT of a saved transaction is merged into
        /// it instead.
        fn import_psbt(self: &MusigAccount, text: &[u8]) -> SavedPsbtImport;
        fn saved_psbt_error(self: &MusigAccount) -> String;
        fn active_attempt(self: &MusigAccount) -> MusigAttemptSnapshot;
        fn clear_active_attempt(self: &MusigAccount) -> bool;
        fn accept_signed_progress(self: &MusigAccount, answer: &str) -> MusigAttemptSnapshot;
        fn finalize_active_attempt(self: &MusigAccount) -> MusigAttemptSnapshot;
        fn activate_saved_psbt(self: &MusigAccount, id: &str) -> SavedPsbtRestore;
        fn reset_active_attempt(self: &MusigAccount) -> MusigAttemptSnapshot;
        fn require_active_restart(self: &MusigAccount) -> MusigAttemptSnapshot;
        fn active_signing_plan(self: &MusigAccount, devices: Vec<LiveDevice>) -> SigningPlan;

        fn funds_snapshot(self: &MusigAccount) -> FundsSnapshot;
        fn labels(self: &MusigAccount) -> LabelSnapshot;
        fn set_label(
            self: &MusigAccount,
            kind: LabelKind,
            id: String,
            text: String,
        ) -> LabelEditResult;
        fn receive_kinds(self: &MusigAccount) -> Vec<ReceiveKind>;
        /// Every generated receive address.
        fn receive_addresses(self: &MusigAccount) -> Vec<ReceiveAddress>;
        fn new_receive_address(self: &MusigAccount, kind: &str) -> ReceiveAddressResult;
        fn craft_transaction(self: &MusigAccount, tx_template: TransactionTemplate)
            -> CraftOutcome;
        fn select_transaction_coins(
            self: &MusigAccount,
            tx_template: TransactionTemplate,
        ) -> CraftOutcome;
        /// The change goes to the next unused change index, which stays unused:
        /// adopt_transaction registers it.
        fn finalize_transaction(
            self: &MusigAccount,
            tx_template: TransactionTemplate,
        ) -> UnsignedPsbt;
        /// Takes `psbt`, a finalize_transaction answer, as the transaction in flight:
        /// registers its change index durably, refusing one another adoption took,
        /// restricts it to `spend` and saves it.
        fn adopt_transaction(
            self: &MusigAccount,
            tx_template: TransactionTemplate,
            spend: MusigSpendLeaf,
            psbt: &[u8],
        ) -> Box<PsbtResult>;
        fn broadcast_report(self: &MusigAccount, transaction: &[u8]) -> BroadcastReport;
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
            kind: BackendErrorKind::None,
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
            kind: match &e {
                bwk_sp::account::AccountError::Network(_) => BackendErrorKind::Network,
                _ => BackendErrorKind::Backend,
            },
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

/// Render `text` (an address) as a single QR code.
pub fn qr_text_image(text: &str) -> ffi::QrImageResult {
    let encoded = bwk_qr::encoder::Encoder::new(bwk_qr::config::Config::default())
        .and_then(|encoder| encoder.encode_text(text));
    match encoded {
        Ok(image) => ffi::QrImageResult {
            is_ok: true,
            error: String::new(),
            data: image.data,
            width: image.width,
            height: image.height,
        },
        Err(e) => ffi::QrImageResult {
            is_ok: false,
            error: format!("failed to render QR code: {e}"),
            data: Vec::new(),
            width: 0,
            height: 0,
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
            wrong_password: false,
        },
        Err(e) => ffi::MnemonicResult {
            is_ok: false,
            error: e.to_string(),
            mnemonic: String::new(),
            wrong_password: false,
        },
    }
}

pub fn bip39_word_is_valid(word: &str) -> bool {
    keys::hot::is_bip39_word(word)
}

pub fn mnemonic_is_valid(mnemonic: &str) -> bool {
    keys::hot::is_valid_mnemonic(mnemonic)
}

/// Validate a recipient address (SP address, legacy Bitcoin address, or hex data).
/// Returns empty string if valid, or error message if invalid.
pub fn validate_address(address: String) -> String {
    if address.is_empty() {
        return String::new();
    }
    // No network here: an address valid on any network passes.
    let networks = [
        bitcoin::Network::Bitcoin,
        bitcoin::Network::Testnet,
        bitcoin::Network::Signet,
        bitcoin::Network::Regtest,
    ];
    let mut error = String::new();
    for network in networks {
        match bwk_sp::receiver::RecipientAddress::parse(&address, network) {
            Ok(_) => return String::new(),
            Err(e) => error = e.to_string(),
        }
    }
    error
}

/// Connect over `scheme` and answer a server.version handshake, the client ready for
/// the next request.
fn electrum_connect(
    host: &str,
    port: u16,
    scheme: ElectrumScheme,
    verify_certificate: bool,
) -> Result<bwk_sp::bwk::bwk_electrum::raw_client::Client, error::BackendFailure> {
    use std::time::Duration;

    let ssl = matches!(scheme, ElectrumScheme::Ssl);
    let mut client = bwk_sp::bwk::bwk_electrum::raw_client::Client::new_ssl_maybe(host, port, ssl)
        .read_timeout(Some(Duration::from_secs(5)))
        .write_timeout(Some(Duration::from_secs(5)))
        .certificate_check(config::certificate_check(verify_certificate))
        .map_err(|e| {
            error::BackendFailure::new(
                BackendErrorKind::InvalidInput,
                format!("Invalid address: {e}"),
            )
        })?;

    client
        .try_connect(Some(Duration::from_secs(5)))
        .map_err(|e| {
            error::BackendFailure::new(BackendErrorKind::Network, format!("Connection failed: {e}"))
        })?;

    let request = bwk_sp::bwk::bwk_electrum::electrum::request::Request::version(
        "silent".to_string(),
        "1.4".to_string(),
    );
    client.try_send(&request).map_err(|e| {
        error::BackendFailure::new(
            BackendErrorKind::Network,
            format!("Failed to send version request: {e}"),
        )
    })?;

    let response = client.recv_str().map_err(|e| {
        error::BackendFailure::new(
            BackendErrorKind::Network,
            format!("No response from server: {e}"),
        )
    })?;

    let response: serde_json::Value = serde_json::from_str(response.trim()).map_err(|e| {
        error::BackendFailure::new(
            BackendErrorKind::Electrum,
            format!("Invalid Electrum version response: {e}"),
        )
    })?;
    if response.get("id") != Some(&serde_json::json!(request.id))
        || response.get("error").is_some_and(|error| !error.is_null())
        || !response
            .get("result")
            .and_then(serde_json::Value::as_array)
            .is_some_and(|result| {
                result.len() == 2 && result.iter().all(serde_json::Value::is_string)
            })
    {
        return Err(error::BackendFailure::new(
            BackendErrorKind::Electrum,
            "Electrum server refused the version handshake",
        ));
    }
    Ok(client)
}

/// Render an electrum address the way bwk expects it: the scheme rides as an
/// `ssl://` prefix on the host (see config::parse_electrum_url).
fn electrum_url(host: &str, port: u16, scheme: ElectrumScheme) -> String {
    match scheme {
        ElectrumScheme::Ssl => format!("ssl://{host}:{port}"),
        ElectrumScheme::Tcp => format!("{host}:{port}"),
    }
}

/// The host, port and schemes to try for `address`: the stated scheme as-is, else
/// plaintext then SSL.
fn electrum_candidates(address: &str) -> Result<(String, u16, Vec<ElectrumScheme>), String> {
    let (host, port, scheme) = bwk_sp::bwk::bwk_electrum::url::parse_electrum_url(address)
        .map_err(|e| format!("Invalid address '{address}': {e}"))?;
    let Some(host) = host else {
        return Err(format!(
            "Invalid address format '{address}': expected host:port"
        ));
    };
    let Some(port) = port else {
        // A port that is not a u16 is kept on the host rather than reported, so a
        // leftover ':' means the port is bad and not simply missing.
        return Err(match host.rsplit_once(':') {
            Some((_, bad)) => format!("Invalid port '{bad}' in '{address}': must be 1 to 65535"),
            None => format!("Invalid address format '{address}': expected host:port"),
        });
    };
    let schemes = if address.contains("://") {
        vec![scheme]
    } else {
        vec![ElectrumScheme::Tcp, ElectrumScheme::Ssl]
    };
    Ok((host, port, schemes))
}

/// Test Electrum server connectivity with a server.version handshake, checking the
/// server's TLS certificate unless `verify_certificate` is false (a self-signed one).
///
/// When the address states a scheme it is used as-is, otherwise plaintext then
/// SSL are tried and the first to answer wins. Returns the URL that connected,
/// so the caller can store the resolved scheme.
pub fn test_electrum(address: String, verify_certificate: bool) -> ffi::ConnectionResult {
    log::info!("test_electrum()");

    let err_conn = |kind, error: String| ffi::ConnectionResult {
        kind,
        is_ok: false,
        error,
        url: String::new(),
    };

    let (host, port, candidates) = match electrum_candidates(&address) {
        Ok(parsed) => parsed,
        Err(e) => return err_conn(BackendErrorKind::InvalidInput, e),
    };

    let mut last_error = None;
    for candidate in candidates {
        match electrum_connect(&host, port, candidate, verify_certificate) {
            Ok(_) => {
                return ffi::ConnectionResult {
                    kind: BackendErrorKind::None,
                    is_ok: true,
                    error: String::new(),
                    url: electrum_url(&host, port, candidate),
                }
            }
            Err(e) => last_error = Some(e),
        }
    }
    match last_error {
        Some(error) => err_conn(error.kind, error.message),
        None => err_conn(
            BackendErrorKind::InvalidInput,
            "No Electrum transport candidates".into(),
        ),
    }
}

/// The height of the chain tip the Electrum server at `address` follows, from its
/// blockchain.headers.subscribe answer. Blocking.
pub fn electrum_tip_height(address: String, verify_certificate: bool) -> ffi::ElectrumTipResult {
    let err_tip = |error: String| ffi::ElectrumTipResult {
        is_ok: false,
        error,
        height: 0,
    };
    let (host, port, candidates) = match electrum_candidates(&address) {
        Ok(parsed) => parsed,
        Err(e) => return err_tip(e),
    };

    let mut last_error = String::new();
    for candidate in candidates {
        let mut client = match electrum_connect(&host, port, candidate, verify_certificate) {
            Ok(client) => client,
            Err(e) => {
                last_error = e.message;
                continue;
            }
        };
        let request =
            bwk_sp::bwk::bwk_electrum::electrum::request::Request::subscribe_headers().id(1);
        if let Err(e) = client.try_send(&request) {
            last_error = format!("Failed to send headers request: {e}");
            continue;
        }
        return match client.recv_str() {
            Ok(response) => match tip_height(&response) {
                Some(height) => ffi::ElectrumTipResult {
                    is_ok: true,
                    error: String::new(),
                    height,
                },
                None => err_tip(format!("No tip height in the answer: {}", response.trim())),
            },
            Err(e) => err_tip(format!("No response from server: {e}")),
        };
    }
    err_tip(last_error)
}

/// `result.height` of a blockchain.headers.subscribe answer.
fn tip_height(response: &str) -> Option<u32> {
    let value: serde_json::Value = serde_json::from_str(response.trim()).ok()?;
    value["result"]["height"]
        .as_u64()
        .and_then(|height| u32::try_from(height).ok())
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
            };
        }
    };

    let blindbit_connect = json["blindbit_connect"].as_str().unwrap_or_default();

    if blindbit_connect.is_empty() {
        return RegtestDefaults {
            is_ok: false,
            error: "Missing fields in API response".to_string(),
            blindbit_url: String::new(),
        };
    }

    RegtestDefaults {
        is_ok: true,
        error: String::new(),
        blindbit_url: format!("http://{blindbit_connect}"),
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

pub fn save_descriptor_backup(
    descriptor: &str,
    path: &str,
    store_common_paths: bool,
) -> ffi::DescriptorBackupResult {
    match backup::write_descriptor_backup(
        descriptor,
        std::path::Path::new(path),
        store_common_paths,
    ) {
        Ok(written) => ffi::DescriptorBackupResult {
            is_ok: true,
            error: String::new(),
            excluded: written.excluded,
        },
        Err(e) => ffi::DescriptorBackupResult {
            is_ok: false,
            error: e,
            excluded: Vec::new(),
        },
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
pub use account_storage::{
    delete_musig_account, delete_sp_account, musig_account_delete_check, sp_account_delete_check,
};

pub use crate::app_state::{
    app_active_theme, app_begin_plugin_removal, app_disable_plugin, app_enabled_plugins,
    app_fee_preferences, app_language, app_last_endpoint, app_plugin_access,
    app_plugin_access_history, app_plugin_access_request, app_plugin_access_revision,
    app_plugin_answers, app_plugin_grants, app_plugin_history, app_price_currency,
    app_price_source, app_qr_camera_contrast_level, app_qr_display_levels, app_removing_plugins,
    app_set_active_theme, app_set_fee_preferences, app_set_language, app_set_last_endpoint,
    app_set_plugin_access, app_set_plugin_access_if_current, app_set_plugin_enabled,
    app_set_price_selection, app_set_qr_camera_contrast_level, app_set_qr_display_levels,
};
pub use account::{
    new_account, new_account_offline, Account, NotificationReceiver, Poll, PsbtResult,
};
pub use bbqr::{bbqr_frames, bbqr_scanner_new, bbqr_text_scanner_new, BbqrFrames, BbqrScanner};
pub use camera::{camera_list, camera_open, CameraHandle};
pub use config::{
    config_from_file, create_musig_account, create_sp_account, delete_config, list_configs,
    list_module_configs, new_config, segwit_descriptor_for, set_datadir, taproot_descriptor_for,
    Config, CONFIG_FILE,
};
pub use coordination::{
    coordination_accept, coordination_prepare, coordination_release, transaction_broadcast,
    transaction_finalize,
};
pub use endpoint::{
    musig_endpoint_change, new_endpoint_retry, sp_endpoint_change, sp_startup_endpoint_change,
    take_musig_endpoint_account, take_sp_endpoint_account, EndpointRetry, MusigEndpointChange,
    SpEndpointChange,
};
pub use error::{notification_error_kind, validation_error_kind};
pub use feed::{new_feed_state, FeedState};
pub use ffi::{
    AccountCreationResult, AmountParseResult, BackendErrorKind, BackendInfo, BackendServer,
    BroadcastOutcome, BroadcastReport, CameraFrameResult, CameraListResult, ConnectionResult,
    CoordinationResult, CraftOutcome, EndpointPhase, EndpointResult, EndpointSetting,
    FeeParseResult, FeedPrice, FeedQuote, FeedSelection, FeedSourceState, FingerprintsResult,
    FundsSnapshot, KeyDescriptor, KeyDevice, KeyFileFailure, KeyInfo, KeyInfoResult, KeyOpResult,
    KeyRouteKind, KeyUnusableReason, KeyXpub, LabelEditResult, LabelEntry, LabelKind,
    LabelSnapshot, LiveDevice, LogLevel, MnemonicResult, MusigAttemptSnapshot,
    MusigCreationRequest, MusigFinalizeResult, MusigRound, MusigSpendLeaf, Network, Notification,
    NotificationFlag, Output, PluginAccess, PluginAccessHistory, PluginAccessRequest, PluginGrant,
    PluginGrantedAccess, PluginMetadata, PluginMetadataField, PluginMetadataKind,
    PluginRemovalEntry, PluginRemovalListResult, PluginRemovalPhase, PolicyCompileResult,
    PolicyDraft, PolicyDraftError, PolicyDraftResult, PolicyDurationUnit, PolicyKey, PolicyKeyType,
    PolicyLocktime, PolicyPath, PolicyReview, PolicyReviewKey, PolicyReviewPath, PolicySemantic,
    PolicySpec, PsbtExport, PsbtFileResult, PsbtImport, PsbtPayloadResult, PsbtValidation,
    QrErrorKind, QrMessageResult, QrMusigSignerRoute, QrResponseKind, ReceiveAddress,
    ReceiveAddressResult, ReceiveKind, RegtestDefaults, RetryDecision, RustCoin, RustSelectedInput,
    RustSpendCoin, RustSpendLock, RustSpendVariant, RustTx, RustTxDetails, RustTxIo,
    SavedPsbtAttempt, SavedPsbtEntry, SavedPsbtImport, SavedPsbtInput, SavedPsbtList,
    SavedPsbtMutation, SavedPsbtRecord, SavedPsbtReset, SavedPsbtRestore, SendAmounts, SignResult,
    SigningCandidate, SigningPlan, SigningProgress, SigningStage, SpCreationRequest,
    SpendAvailability, SpendCoinStatus, SpendCoinsResult, SpendError, SpendLockKind, SpendMaturity,
    TransactionSimulation, TransactionTemplate, UnsignedPsbt,
};
pub use hwi::{new_hwi_service, HwiPoll, HwiReceiver, HwiService};
pub use hwi_config::{
    hwi_config_network, hwi_set_config_network, BitboxSettings, HwiConfig, NoiseConfig,
    NoiseConfigData, PersistedBitboxNoiseConfig,
};
pub use keys::{
    bridge::{
        account_xpub_usage, commit_key_file, hot_cached_nonces, hot_create_sessions, hot_reveal,
        hot_seal, hot_sign, hot_sp_descriptor, hot_xpub, key_file_preview, ledger_nonce_count,
        ledger_nonce_fetch, ledger_nonce_fetch_for_signing, ledger_nonce_invalidate,
        ledger_nonce_limit, ledger_nonce_merge, ledger_nonce_plan, ledger_nonce_refresh,
        ledger_nonce_restart, ledger_nonce_sign, merge_signed_psbt, musig_session_count,
        musig_session_limit, musig_session_plan, musig_session_refresh, musig_session_restart,
        new_keys_module, qr_musig_accept, qr_musig_prepare_material, qr_musig_sign_material,
        usb_merge_signed, usb_sign_payload, AccountXpubUsage, HotMaterial, KeyFileCommit,
        KeyFilePreview, KeysModule, LedgerNonceFetch, QrMusigPrepare, QrMusigSign, QrResponse,
        SealedHotKey,
    },
    supports_silent_payments,
};
pub use money::{normalize_fee_rate, parse_btc_amount, parse_fee_input, send_amounts};

impl Default for ffi::PluginMetadataKind {
    fn default() -> Self {
        Self::Text
    }
}

impl Default for ffi::BackendErrorKind {
    fn default() -> Self {
        Self::Other
    }
}
pub use musig::{
    musig_descriptor_fingerprints, musig_first_receive_address, new_musig_account,
    new_musig_account_offline, validate_musig_descriptor, MusigAccount, MUSIG_PLUGIN_ID,
};
pub use plugin_admission::plugin_admission;
pub use plugin_bundle::{
    plugin_bundles, plugin_install, plugin_library, plugin_metadata, plugin_remove,
};
pub use policy::{
    bip87_account_path, compile_policy, policy_duration_blocks, policy_threshold, preview_policy,
    renewable_height, resolve_policy_draft, review_policy,
};
pub use psbt_transfer::export_psbt;
pub use services::{save_endpoints, test_endpoint};
pub use state_crypt::{
    state_clear_file, state_create_with_device, state_create_with_password, state_device_path,
    state_draw_index, state_lock, state_unlock_info, state_unlock_with_device,
    state_unlock_with_password,
};
pub use storage::{
    storage_delivery_error, storage_request, storage_request_cancel, storage_request_forget,
    storage_session_generation, storage_session_open, storage_session_revoke,
};
pub use sync_estimator::{new_sync_estimator, SyncEstimator};
pub use tx_details::test_and_save_endpoint;

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
        let result = test_electrum("electrum.pythcoiner.dev".to_string(), true);
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
        let result = test_electrum("electrum.pythcoiner.dev:500002".to_string(), true);
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
        let result = test_electrum("wss://electrum.pythcoiner.dev:443".to_string(), true);
        assert!(!result.is_ok);
        assert!(result.url.is_empty());
        assert!(
            result.error.contains("unsupported electrum scheme"),
            "unexpected error: {}",
            result.error
        );
    }

    #[test]
    fn test_electrum_refuses_a_json_rpc_error_or_wrong_response_id() {
        use std::{
            io::{BufRead, BufReader, Write},
            net::TcpListener,
            thread,
            time::Duration,
        };

        for wrong_id in [false, true] {
            let listener = TcpListener::bind("127.0.0.1:0").expect("isolated version server");
            let address = listener.local_addr().expect("local address");
            let server = thread::spawn(move || {
                let (mut stream, _) = listener.accept().expect("test client connects");
                stream
                    .set_read_timeout(Some(Duration::from_secs(5)))
                    .expect("read timeout");
                let mut line = String::new();
                BufReader::new(stream.try_clone().expect("socket clone"))
                    .read_line(&mut line)
                    .expect("version request");
                let request: serde_json::Value = serde_json::from_str(&line).expect("JSON request");
                let response = if wrong_id {
                    serde_json::json!({"jsonrpc":"2.0", "id":12345, "result":["server", "1.4"]})
                } else {
                    serde_json::json!({"jsonrpc":"2.0", "id":request["id"], "error":{"code":-1, "message":"refused"}})
                };
                writeln!(stream, "{response}").expect("version response");
            });
            let result = test_electrum(format!("tcp://{address}"), true);
            server.join().expect("version server finished");
            assert!(
                !result.is_ok,
                "wrong_id={wrong_id}: a failed handshake is not a passing test"
            );
            assert!(
                result.error.contains("refused the version handshake"),
                "handshake refusal reported: {}",
                result.error
            );
        }
    }

    #[test]
    fn tip_height_reads_a_headers_subscribe_answer() {
        assert_eq!(
            tip_height(r#"{"jsonrpc":"2.0","id":1,"result":{"height":905210,"hex":"00"}}"#),
            Some(905210)
        );
        assert_eq!(
            tip_height(r#"{"jsonrpc":"2.0","id":1,"error":"busy"}"#),
            None
        );
        assert_eq!(tip_height("not json"), None);
    }

    #[test]
    fn qr_text_image_renders_an_address() {
        let result = qr_text_image("bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080");

        assert!(result.is_ok, "unexpected error: {}", result.error);
        assert!(result.width > 0, "width should be positive");
        assert_eq!(
            result.width, result.height,
            "a QR code is square, got {}x{}",
            result.width, result.height
        );
        assert_eq!(
            result.data.len(),
            (result.width * result.height) as usize,
            "one byte per pixel"
        );
    }

    #[test]
    fn qr_text_image_rejects_text_too_long_for_one_code() {
        let result = qr_text_image(&"a".repeat(10_000));

        assert!(!result.is_ok, "10000 bytes cannot fit a single QR code");
        assert!(result.data.is_empty(), "no pixels on failure");
    }
}
