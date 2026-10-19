//! Silent - Bitcoin Silent Payment wallet backed by BlindBit
//!
//! This library provides C++ bindings for the bwk-sp Silent Payment implementation.

pub mod account;
pub mod app_state;
pub mod backup;
pub mod bbqr;
pub mod camera;
pub mod config;
pub mod hwi;
pub mod hwi_config;
pub mod image;
pub mod keys;
pub mod musig;
pub mod policy;
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
    }

    /// Kind of a decoded and validated QR response.
    #[derive(Debug, Clone, Copy)]
    pub enum QrResponseKind {
        Xpubs,
        Registration,
        AddressVerified,
        SignedPsbt,
        SpAccount,
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
        /// The capability bits it supports (bit 2 silent payments, bit 3 MuSig2), 0
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
        /// bwk-qr capability bits, 0 for a USB device.
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
        /// bwk-qr capability bits, 0 for a USB device.
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
        /// The listener started on it: false for an empty or unparsable url.
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
    #[derive(Debug, Clone)]
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

        /// The height of the chain tip the Electrum server at `address` follows.
        /// Blocking: call it off the GUI thread.
        fn electrum_tip_height(address: String, verify_certificate: bool) -> ElectrumTipResult;

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

        /// Get spendable coins summary.
        fn spendable_coins(self: &Account) -> CoinState;

        /// Update a coin's label.
        fn update_coin_label(self: &mut Account, outpoint: String, label: String) -> bool;

        /// Get payment history.
        fn payment_history(self: &Account) -> Vec<RustTx>;

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
        /// Marks used every key xpub the accounts of the network are built on, caching
        /// it when the key's cache lacks it.
        fn mark_account_xpubs_used(self: &mut KeysModule) -> KeyOpResult;
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
        /// Saves the endpoint to the config and restarts the listener.
        fn set_electrum_url(self: &mut MusigAccount, url: String) -> MusigElectrumUrlResult;

        fn try_recv(self: &mut MusigAccount) -> Box<Poll>;
        fn take_receiver(self: &mut MusigAccount) -> Box<NotificationReceiver>;

        fn balance(self: &MusigAccount) -> u64;
        fn new_addr(self: &mut MusigAccount) -> String;
        fn coins(self: &MusigAccount) -> Vec<RustCoin>;
        fn spendable_coins(self: &MusigAccount) -> CoinState;
        fn update_coin_label(self: &mut MusigAccount, outpoint: String, label: String) -> bool;
        fn payment_history(self: &MusigAccount) -> Vec<RustTx>;
        fn address_verify_info(self: &MusigAccount, address: &str) -> AddressVerifyInfo;

        fn simulate_transaction(
            self: &MusigAccount,
            tx_template: TransactionTemplate,
        ) -> TransactionSimulation;
        /// Every spend path for the coins `outpoints`, `spendable` at the current tip.
        fn spend_leaves(self: &MusigAccount, outpoints: Vec<String>) -> MusigSpendLeaves;
        /// Prepares the PSBT, then restricts it to `spend` (Account::restrict_spend_path).
        fn prepare_transaction(
            self: &MusigAccount,
            tx_template: TransactionTemplate,
            spend: MusigSpendLeaf,
        ) -> Box<PsbtResult>;
        /// Per cosigner nonce / partial sig counts of the PSBT.
        fn round_status(self: &MusigAccount, psbt_v2_hex: &str) -> MusigRoundStatus;
        /// Aggregates partial sigs, finalizes; TxResult value is the tx hex.
        fn finalize_psbt(self: &MusigAccount, psbt_v2_hex: &str) -> TxResult;
        /// TxResult value is the txid.
        fn broadcast_transaction(self: &MusigAccount, signed_tx_hex: String) -> TxResult;
        /// Blocking: call from a background thread.
        fn validate_before_sign(self: &MusigAccount, psbt_result: &PsbtResult) -> PsbtValidation;
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
) -> Result<bwk_sp::bwk::bwk_electrum::raw_client::Client, String> {
    use std::time::Duration;

    let ssl = matches!(scheme, ElectrumScheme::Ssl);
    let mut client = bwk_sp::bwk::bwk_electrum::raw_client::Client::new_ssl_maybe(host, port, ssl)
        .read_timeout(Some(Duration::from_secs(5)))
        .write_timeout(Some(Duration::from_secs(5)))
        .certificate_check(config::certificate_check(verify_certificate))
        .map_err(|e| format!("Invalid address: {e}"))?;

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
        Ok(serde_json::Value::Object(_)) => Ok(client),
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

    let err_conn = |error: String| ffi::ConnectionResult {
        is_ok: false,
        error,
        url: String::new(),
    };

    let (host, port, candidates) = match electrum_candidates(&address) {
        Ok(parsed) => parsed,
        Err(e) => return err_conn(e),
    };

    let mut last_error = String::new();
    for candidate in candidates {
        match electrum_connect(&host, port, candidate, verify_certificate) {
            Ok(_) => {
                return ffi::ConnectionResult {
                    is_ok: true,
                    error: String::new(),
                    url: electrum_url(&host, port, candidate),
                }
            }
            Err(e) => last_error = e,
        }
    }
    err_conn(last_error)
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
                last_error = e;
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
pub use account::{new_account, Account, NotificationReceiver, Poll, PsbtResult};
pub use app_state::{
    app_active_theme, app_enabled_plugins, app_language, app_last_endpoint,
    app_qr_camera_contrast_level, app_qr_display_levels, app_set_active_theme, app_set_language,
    app_set_last_endpoint, app_set_plugin_enabled, app_set_qr_camera_contrast_level,
    app_set_qr_display_levels,
};
pub use bbqr::{bbqr_frames, bbqr_scanner_new, bbqr_text_scanner_new, BbqrFrames, BbqrScanner};
pub use camera::{camera_list, camera_open, CameraHandle};
pub use config::{
    config_from_file, delete_config, list_configs, new_config, segwit_descriptor_for, set_datadir,
    taproot_descriptor_for, Config,
};
pub use ffi::{
    BackendInfo, CameraFrameResult, CameraListResult, FingerprintsResult, KeyDescriptor, KeyDevice,
    KeyInfo, KeyInfoResult, KeyOpResult, KeyRouteKind, KeyUnusableReason, KeyXpub, LiveDevice,
    LogLevel, MnemonicResult, MusigRound, Network, Notification, NotificationFlag, Output,
    PolicyCompileResult, PolicyKey, PolicyKeyType, PolicyLocktime, PolicyPath, PolicyReview,
    PolicyReviewKey, PolicyReviewPath, PolicySemantic, PolicySpec, PsbtFileResult,
    PsbtPayloadResult, PsbtValidation, QrErrorKind, QrMessageResult, QrResponseKind,
    RegtestDefaults, SignResult, SigningCandidate, SigningPlan, TransactionSimulation,
    TransactionTemplate,
};
pub use hwi::{new_hwi_service, HwiPoll, HwiReceiver, HwiService};
pub use hwi_config::{
    hwi_config_network, hwi_set_config_network, BitboxSettings, HwiConfig, NoiseConfig,
    NoiseConfigData, PersistedBitboxNoiseConfig,
};
pub use keys::{
    bridge::{
        hot_reveal, hot_seal, hot_sign, hot_sp_descriptor, hot_xpub, merge_signed_psbt,
        new_keys_module, usb_merge_signed, usb_sign_payload, HotMaterial, KeysModule, QrResponse,
        SealedHotKey,
    },
    supports_silent_payments,
};
pub use musig::{
    musig_descriptor_fingerprints, musig_first_receive_address, new_musig_account,
    validate_musig_descriptor, MusigAccount, MUSIG_PLUGIN_ID,
};
pub use policy::{
    bip87_account_path, compile_policy, preview_policy, renewable_height, review_policy,
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
