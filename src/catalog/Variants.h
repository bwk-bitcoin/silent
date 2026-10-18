#pragma once

#include <QString>

namespace catalog {

class Badge;
class Button;
class CopyButton;

// Every button and pill the app shows, one helper per design variant (role, label,
// icon, tooltip, width). Screens pick a helper, they never configure one by hand.

auto btnBack() -> Button *;
auto btnOtherSigner() -> Button *;
auto btnAllSettings() -> Button *;
auto btnStepperMinus() -> Button *;
auto btnStepperPlus() -> Button *;
auto btnSaveAlias() -> Button *;
auto btnAddRouteIcon() -> Button *;
auto btnConfirmAddRoute() -> Button *;
auto btnCancelAddRoute() -> Button *;
auto btnFetchXpub() -> Button *;
auto btnRegisterAccount() -> Button *;
auto btnFetchThisXpub() -> Button *;
auto btnCameraSettings() -> Button *;
auto btnQrSettings() -> Button *;
auto btnOpenFile() -> Button *;
auto btnScanPlugins() -> Button *;
auto btnScanningPlugins() -> Button *;
auto btnDisablePlugin() -> Button *;
auto btnAddKey() -> Button *;
auto btnBroadcast() -> Button *;
auto btnCheckAddress() -> Button *;
auto btnContinue() -> Button *;
auto btnCreate() -> Button *;
auto btnDone() -> Button *;
auto btnGetDescriptorHot() -> Button *;
auto btnGetXpubHot() -> Button *;
auto btnLoadSignedPsbt() -> Button *;
auto btnNextScanTheAnswer() -> Button *;
auto btnPickAnotherFile() -> Button *;
auto btnProceed() -> Button *;
auto btnRegister() -> Button *;
auto btnSaveSettings() -> Button *;
auto btnSaveUnsignedPsbt() -> Button *;
auto btnSealKey() -> Button *;
auto btnShowMnemonic() -> Button *;
auto btnSign() -> Button *;
auto btnSignHot() -> Button *;
auto btnTryAgain() -> Button *;
auto btnAddAKey() -> Button *;
auto btnAddPayment() -> Button *;
auto btnAddPath() -> Button *;
auto btnClear() -> Button *;
auto btnCancelSigned() -> Button *;
auto btnConnectBlindbit() -> Button *;
auto btnConnectElectrum() -> Button *;
auto btnDisconnectBlindbit() -> Button *;
auto btnDisconnectElectrum() -> Button *;
auto btnGenerate() -> Button *;
auto btnGenerateNew() -> Button *;
// Create Account's fetch row: the sp() descriptor, the taproot and the segwit xpub.
auto btnGetSilentPayment() -> Button *;
auto btnGetTaproot() -> Button *;
auto btnGetSegwit() -> Button *;
auto btnOpen() -> Button *;
auto btnOpenKeysTab() -> Button *;
auto btnRevealMnemonic() -> Button *;
auto btnScanAgain() -> Button *;
auto btnTest() -> Button *;
auto btnTestOk() -> Button *;
auto btnTesting() -> Button *;
auto btnDelete() -> Button *;
auto btnRemoveKey() -> Button *;
auto btnAddLabel() -> Button *;
auto btnHideWords() -> Button *;
auto btnShowWords() -> Button *;
auto btnDeleteWallet() -> Button *;
auto btnVerifyOnDevice() -> Button *;
auto btnSettingsGear() -> Button *;
auto btnDismissNotice() -> Button *;
auto btnEditLabel() -> Button *;
auto btnEditLabelRow() -> Button *;
auto btnMoveDown() -> Button *;
auto btnMoveUp() -> Button *;
auto btnRemovePayment() -> Button *;
auto btnRemovePath() -> Button *;
// The Edit Spend Path view of the Multikey creator.
auto btnRemoveSpendPath() -> Button *;
auto btnSaveSpendPath() -> Button *;
// The creator's Review step: the main action until the descriptor is saved, then a
// default one to save it again.
auto btnBackupDescriptorPrimary() -> Button *;
auto btnBackupDescriptor() -> Button *;
auto btnRegisterDescriptor() -> Button *;
auto btnScanAddress() -> Button *;
auto btnRemoveRoute() -> Button *;
auto btnRemoveXpub() -> Button *;
auto btnSaveLabel() -> Button *;
auto btnVerifyOnDeviceSm() -> Button *;

// A copy button flips to a check and "Copied" for a moment after each click.
auto btnCopyValue() -> CopyButton *;
auto btnCopySm() -> CopyButton *;
auto btnCopyAddress() -> CopyButton *;

// Data-labelled entries take their text.
auto btnAwaitingPayment(int count) -> Button *;
auto btnOpenWallet(const QString &name) -> Button *;
// A launcher create button, labelled by its creator entry.
auto btnCreateAccount() -> Button *;

auto pillDeviceReady() -> Badge *;
auto pillDeviceLocked() -> Badge *;
auto pillDeviceUnsupported() -> Badge *;
auto pillKeyConnected() -> Badge *;
auto pillKeyHot() -> Badge *;
auto pillKeyOffline() -> Badge *;
auto pillAccountRegistered() -> Badge *;
auto pillSignerFirst() -> Badge *;
auto pillSignerReady() -> Badge *;
auto pillSignerSigned() -> Badge *;
// The MuSig2 Sign modal: a cosigner's part still needed, its round 1 nonce added.
auto pillMusigNeeded() -> Badge *;
auto pillMusigNonceAdded() -> Badge *;
auto pillMusigOnDevice() -> Badge *;
auto pillSignerWaiting() -> Badge *;
auto pillSignerDisconnected() -> Badge *;
auto pillSignerNoHot() -> Badge *;
auto pillSignerNoRoute() -> Badge *;
auto pillSignerUnregistered() -> Badge *;
auto pillSignerNotInPath() -> Badge *;
auto pillSignerUnsupported() -> Badge *;
auto pillSignerNotInWallet() -> Badge *;
auto pillSdStepDone() -> Badge *;
auto pillTxSigned() -> Badge *;
auto pillXpubUsed() -> Badge *;
auto pillXpubUnused() -> Badge *;
// The Multikey creator's Xpubs step: one per key of the spend paths.
auto pillXpubFetched() -> Badge *;
auto pillXpubMissing() -> Badge *;
// Its Register step: one per signer.
auto pillRegRegistered() -> Badge *;
auto pillRegPending() -> Badge *;
auto pillRegRegistering() -> Badge *;
auto pillRegChecking() -> Badge *;
auto pillRegFailed() -> Badge *;
auto pillRegNotNeeded() -> Badge *;
// Its Verify step: one per signer.
auto pillAddrVerified() -> Badge *;
auto pillAddrUnchecked() -> Badge *;
auto pillAddrMismatch() -> Badge *;
auto pillAddrNoScreen() -> Badge *;
auto pillPluginUnloading() -> Badge *;

auto pillNetwork(const QString &network) -> Badge *;

} // namespace catalog
