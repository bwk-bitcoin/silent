#pragma once

#include <QString>

namespace catalog {

class Badge;
class Button;
class CopyButton;

// Every button and pill the app shows, one helper per design variant (role, label,
// icon, tooltip, width). Screens pick a helper, they never configure one by hand.

auto btnBack() -> Button *;
auto btnPrevious() -> Button *;
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
auto btnUnlocking() -> Button *;
auto btnAddAKey() -> Button *;
auto btnAddPayment() -> Button *;
auto btnClear() -> Button *;
auto btnConnectBlindbit() -> Button *;
auto btnConnectElectrum() -> Button *;
auto btnDisconnectBlindbit() -> Button *;
auto btnDisconnectElectrum() -> Button *;
auto btnGenerate() -> Button *;
auto btnGenerateNew() -> Button *;
auto btnGetAgain() -> Button *;
auto btnGetDescriptor() -> Button *;
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
auto btnRemoveRoute() -> Button *;
auto btnSaveLabel() -> Button *;
auto btnVerifyOnDeviceSm() -> Button *;
auto btnCreateNewWallet() -> Button *;

// A copy button flips to a check and "Copied" for a moment after each click.
auto btnCopyValue() -> CopyButton *;
auto btnCopySm() -> CopyButton *;
auto btnCopyAddress() -> CopyButton *;

// Data-labelled entries take their text.
auto btnAwaitingPayment(int count) -> Button *;
auto btnOpenWallet(const QString &name) -> Button *;

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
auto pillSignerWaiting() -> Badge *;
auto pillSignerDisconnected() -> Badge *;
auto pillSignerNoHot() -> Badge *;
auto pillSignerNoRoute() -> Badge *;
auto pillSignerUnregistered() -> Badge *;
auto pillSdStepDone() -> Badge *;
auto pillTxSigned() -> Badge *;
auto pillXpubUsed() -> Badge *;
auto pillXpubUnused() -> Badge *;
auto pillPluginUnloading() -> Badge *;

auto pillNetwork(const QString &network) -> Badge *;

} // namespace catalog
