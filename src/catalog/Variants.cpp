#include "catalog/Variants.h"

#include "catalog/Button.h"
#include "catalog/CopyButton.h"
#include "catalog/display/Badge.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"

namespace catalog {

namespace {

struct ButtonSpec {
    ButtonRole role = ButtonRole::Default;
    QString label;
    IconFn icon = nullptr;
    int iconSize = 0;
    QString tooltip;
    BtnWidth width = BtnWidth::M;
};

// The glyph takes the label color of its role.
auto iconColor(ButtonRole role) -> IconColor {
    switch (role) {
    case ButtonRole::Primary:
    case ButtonRole::Destructive:
        return IconColor::OnAccent;
    default:
        return IconColor::Default;
    }
}

auto make(const ButtonSpec &spec) -> Button * {
    auto *button = new Button(spec.label, spec.role);
    if (spec.icon != nullptr) {
        button->setIcon(spec.icon(iconColor(spec.role)));
        button->setIconSize(QSize(spec.iconSize, spec.iconSize));
    }
    button->setToolTip(spec.tooltip);
    button->setWidth(spec.width);
    return button;
}

auto makeCopy(CopyButton::Size size, int icon_size, const QString &tooltip) -> CopyButton * {
    auto *button = new CopyButton(size);
    button->setIconSize(QSize(icon_size, icon_size));
    button->setToolTip(tooltip);
    return button;
}

auto makePill(Badge::Role role, const QString &label, PillWidth width) -> Badge * {
    auto *pill = new Badge(label, role);
    pill->setWidth(width);
    return pill;
}

} // namespace

auto btnBack() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-back"),
                 .icon = icon::chevronLeft,
                 .iconSize = 14,
                 .width = BtnWidth::M});
}

auto btnOtherSigner() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-other-signer"),
                 .icon = icon::chevronLeft,
                 .iconSize = 14,
                 .width = BtnWidth::L});
}

auto btnAllSettings() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-all-settings"),
                 .icon = icon::chevronLeft,
                 .iconSize = 14,
                 .width = BtnWidth::L});
}

auto btnStepperMinus() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::minus,
                 .iconSize = 14,
                 .tooltip = TR("btn-less"),
                 .width = BtnWidth::Icon});
}

auto btnStepperPlus() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::plus,
                 .iconSize = 14,
                 .tooltip = TR("btn-more"),
                 .width = BtnWidth::Icon});
}

auto btnSaveAlias() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::check,
                 .iconSize = 16,
                 .tooltip = TR("btn-save-alias"),
                 .width = BtnWidth::Icon});
}

auto btnAddRouteIcon() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::plus,
                 .iconSize = 14,
                 .tooltip = TR("btn-add-route"),
                 .width = BtnWidth::Icon});
}

auto btnConfirmAddRoute() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::check,
                 .iconSize = 14,
                 .tooltip = TR("btn-add-this-route"),
                 .width = BtnWidth::Icon});
}

auto btnCancelAddRoute() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::x,
                 .iconSize = 14,
                 .tooltip = TR("common-cancel"),
                 .width = BtnWidth::Icon});
}

auto btnFetchXpub() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::plus,
                 .iconSize = 14,
                 .tooltip = TR("btn-fetch-xpubs-from-the-device"),
                 .width = BtnWidth::Icon});
}

auto btnRegisterAccount() -> Button * {
    return make({.role = ButtonRole::Default, .label = TR("btn-register"), .width = BtnWidth::M});
}

auto btnFetchThisXpub() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-fetch-xpub"), .width = BtnWidth::M});
}

auto btnCameraSettings() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::settings,
                 .iconSize = 16,
                 .tooltip = TR("btn-camera-settings"),
                 .width = BtnWidth::Icon});
}

auto btnQrSettings() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::settings,
                 .iconSize = 16,
                 .tooltip = TR("btn-qr-settings"),
                 .width = BtnWidth::Icon});
}

auto btnOpenFile() -> Button * {
    return make({.role = ButtonRole::Primary,
                 .label = TR("btn-open-file"),
                 .icon = icon::folder,
                 .iconSize = 16,
                 .width = BtnWidth::L});
}

auto btnScanPlugins() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-scan-for-plugins"),
                 .icon = icon::refresh,
                 .iconSize = 14,
                 .width = BtnWidth::XL});
}

auto btnScanningPlugins() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-scanning"),
                 .icon = icon::refresh,
                 .iconSize = 14,
                 .width = BtnWidth::XL});
}

auto btnDisablePlugin() -> Button * {
    return make(
        {.role = ButtonRole::Destructive, .label = TR("btn-disable-plugin"), .width = BtnWidth::L});
}

auto btnPluginAccess() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-plugin-access"),
                 .icon = icon::lock,
                 .iconSize = 14,
                 .width = BtnWidth::M});
}

auto btnForgetAccess() -> Button * {
    return make(
        {.role = ButtonRole::Default, .label = TR("btn-forget-access"), .width = BtnWidth::M});
}

auto btnAllowAccess() -> Button * {
    return make(
        {.role = ButtonRole::Primary, .label = TR("btn-allow-access"), .width = BtnWidth::M});
}

auto btnDenyAccess() -> Button * {
    return make(
        {.role = ButtonRole::Default, .label = TR("btn-deny-access"), .width = BtnWidth::M});
}

auto btnAddKey() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-add-key"), .width = BtnWidth::M});
}

auto btnBroadcast() -> Button * {
    return make(
        {.role = ButtonRole::Primary, .label = TR("common-broadcast"), .width = BtnWidth::M});
}

auto btnCheckAddress() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-check"), .width = BtnWidth::M});
}

auto btnContinue() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-continue"), .width = BtnWidth::M});
}

auto btnCreate() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-create"), .width = BtnWidth::M});
}

auto btnDone() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-done"), .width = BtnWidth::M});
}

auto btnGetDescriptorHot() -> Button * {
    return make(
        {.role = ButtonRole::Primary, .label = TR("btn-get-descriptor"), .width = BtnWidth::L});
}

auto btnGetXpubHot() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-get-xpub"), .width = BtnWidth::L});
}

auto btnLoadSignedPsbt() -> Button * {
    return make({.role = ButtonRole::Primary,
                 .label = TR("btn-load-signed-psbt"),
                 .icon = icon::upload,
                 .iconSize = 16,
                 .width = BtnWidth::XL});
}

auto btnNextScanTheAnswer() -> Button * {
    return make({.role = ButtonRole::Primary,
                 .label = TR("btn-next-scan-the-answer"),
                 .width = BtnWidth::XL});
}

auto btnPickAnotherFile() -> Button * {
    return make({.role = ButtonRole::Primary,
                 .label = TR("btn-pick-another-file"),
                 .icon = icon::upload,
                 .iconSize = 16,
                 .width = BtnWidth::XL});
}

auto btnProceed() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("common-proceed"), .width = BtnWidth::M});
}

auto btnRegister() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-register"), .width = BtnWidth::M});
}

auto btnSaveSettings() -> Button * {
    return make(
        {.role = ButtonRole::Primary, .label = TR("btn-save-settings"), .width = BtnWidth::L});
}

auto btnSaveUnsignedPsbt() -> Button * {
    return make({.role = ButtonRole::Primary,
                 .label = TR("btn-save-unsigned-psbt"),
                 .icon = icon::download,
                 .iconSize = 16,
                 .width = BtnWidth::XL});
}

auto btnSealKey() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-seal-key"), .width = BtnWidth::M});
}

auto btnShowMnemonic() -> Button * {
    return make(
        {.role = ButtonRole::Primary, .label = TR("btn-show-mnemonic"), .width = BtnWidth::L});
}

auto btnSign() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-sign"), .width = BtnWidth::M});
}

auto btnSignHot() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-sign"), .width = BtnWidth::M});
}

auto btnTryAgain() -> Button * {
    return make({.role = ButtonRole::Primary, .label = TR("btn-try-again"), .width = BtnWidth::M});
}

auto btnAddAKey() -> Button * {
    return make({.role = ButtonRole::Default, .label = TR("btn-add-a-key"), .width = BtnWidth::M});
}

auto btnAddPayment() -> Button * {
    return make(
        {.role = ButtonRole::Default, .label = TR("btn-add-payment"), .width = BtnWidth::L});
}

auto btnAddPath() -> Button * {
    return make(
        {.role = ButtonRole::Default, .label = TR("musig-create-add-path"), .width = BtnWidth::L});
}

auto btnClear() -> Button * {
    return make({.role = ButtonRole::Default, .label = TR("common-clear"), .width = BtnWidth::M});
}

auto btnCancelSigned() -> Button * {
    return make(
        {.role = ButtonRole::Destructive, .label = TR("common-cancel"), .width = BtnWidth::M});
}

auto btnConnectBlindbit() -> Button * {
    return make(
        {.role = ButtonRole::Default, .label = TR("btn-connect-blindbit"), .width = BtnWidth::L});
}

auto btnConnectElectrum() -> Button * {
    return make(
        {.role = ButtonRole::Default, .label = TR("btn-connect-electrum"), .width = BtnWidth::L});
}

auto btnDisconnectBlindbit() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-disconnect-blindbit"),
                 .width = BtnWidth::L});
}

auto btnDisconnectElectrum() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-disconnect-electrum"),
                 .width = BtnWidth::L});
}

auto btnGenerate() -> Button * {
    return make(
        {.role = ButtonRole::Default, .label = TR("common-generate"), .width = BtnWidth::M});
}

auto btnGenerateNew() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-generate-new"),
                 .icon = icon::refresh,
                 .iconSize = 15,
                 .width = BtnWidth::L});
}

auto btnGetSilentPayment() -> Button * {
    return make(
        {.role = ButtonRole::Default, .label = TR("btn-get-silent-payment"), .width = BtnWidth::L});
}

auto btnGetTaproot() -> Button * {
    return make(
        {.role = ButtonRole::Default, .label = TR("btn-get-taproot"), .width = BtnWidth::M});
}

auto btnGetSegwit() -> Button * {
    return make({.role = ButtonRole::Default, .label = TR("btn-get-segwit"), .width = BtnWidth::M});
}

auto btnOpen() -> Button * {
    return make({.role = ButtonRole::Default, .label = TR("btn-open"), .width = BtnWidth::M});
}

auto btnOpenKeysTab() -> Button * {
    return make(
        {.role = ButtonRole::Default, .label = TR("btn-open-keys-tab"), .width = BtnWidth::L});
}

auto btnRevealMnemonic() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-reveal-mnemonic"),
                 .icon = icon::eye,
                 .iconSize = 16,
                 .width = BtnWidth::XL});
}

auto btnScanAgain() -> Button * {
    return make({.role = ButtonRole::Default, .label = TR("btn-scan-again"), .width = BtnWidth::M});
}

auto btnTest() -> Button * {
    return make({.role = ButtonRole::Default, .label = TR("common-test"), .width = BtnWidth::M});
}

auto btnTestOk() -> Button * {
    return make({.role = ButtonRole::Default, .label = TR("btn-test-ok"), .width = BtnWidth::M});
}

auto btnTesting() -> Button * {
    return make({.role = ButtonRole::Default, .label = TR("common-testing"), .width = BtnWidth::M});
}

auto btnDelete() -> Button * {
    return make(
        {.role = ButtonRole::Destructive, .label = TR("common-delete"), .width = BtnWidth::M});
}

auto btnRemoveKey() -> Button * {
    return make(
        {.role = ButtonRole::Destructive, .label = TR("btn-remove-key"), .width = BtnWidth::M});
}

auto btnAddLabel() -> Button * {
    return make({.role = ButtonRole::Inline,
                 .label = TR("btn-add-a-label"),
                 .icon = icon::plus,
                 .iconSize = 14,
                 .width = BtnWidth::Auto});
}

auto btnHideWords() -> Button * {
    return make({.role = ButtonRole::Inline,
                 .label = TR("btn-hide-words"),
                 .icon = icon::eyeOff,
                 .iconSize = 14,
                 .width = BtnWidth::Auto});
}

auto btnShowWords() -> Button * {
    return make({.role = ButtonRole::Inline,
                 .label = TR("btn-show-words"),
                 .icon = icon::eye,
                 .iconSize = 14,
                 .width = BtnWidth::Auto});
}

auto btnDeleteWallet() -> Button * {
    return make({.role = ButtonRole::Icon,
                 .icon = icon::trash,
                 .iconSize = 20,
                 .tooltip = TR("btn-delete-wallet"),
                 .width = BtnWidth::IconLg});
}

auto btnVerifyOnDevice() -> Button * {
    return make(
        {.role = ButtonRole::Icon, .icon = icon::usb, .iconSize = 20, .width = BtnWidth::IconLg});
}

auto btnSettingsGear() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::settings,
                 .iconSize = 16,
                 .tooltip = TR("btn-settings"),
                 .width = BtnWidth::Icon});
}

auto btnDismissNotice() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::x,
                 .iconSize = 14,
                 .tooltip = TR("btn-dismiss"),
                 .width = BtnWidth::Icon});
}

auto btnEditLabel() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::pencil,
                 .iconSize = 14,
                 .tooltip = TR("btn-edit-label"),
                 .width = BtnWidth::Icon});
}

auto btnEditLabelRow() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::pencil,
                 .iconSize = 15,
                 .width = BtnWidth::Icon});
}

auto btnMoveDown() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::arrowDown,
                 .iconSize = 14,
                 .tooltip = TR("btn-move-down"),
                 .width = BtnWidth::Icon});
}

auto btnMoveUp() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::arrowUp,
                 .iconSize = 14,
                 .tooltip = TR("btn-move-up"),
                 .width = BtnWidth::Icon});
}

auto btnScanAddress() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::qr,
                 .iconSize = 16,
                 .tooltip = TR("btn-scan-address"),
                 .width = BtnWidth::Icon});
}

auto btnRemovePayment() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::x,
                 .iconSize = 16,
                 .tooltip = TR("btn-remove-payment"),
                 .width = BtnWidth::Icon});
}

auto btnRemovePath() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::x,
                 .iconSize = 16,
                 .tooltip = TR("btn-remove-path"),
                 .width = BtnWidth::Icon});
}

auto btnRemoveSpendPath() -> Button * {
    return make(
        {.role = ButtonRole::Destructive, .label = TR("btn-remove-path"), .width = BtnWidth::M});
}

auto btnSaveSpendPath() -> Button * {
    return make(
        {.role = ButtonRole::Primary, .label = TR("btn-save-spend-path"), .width = BtnWidth::M});
}

auto btnBackupDescriptorPrimary() -> Button * {
    return make({.role = ButtonRole::Primary,
                 .label = TR("btn-backup-descriptor"),
                 .icon = icon::download,
                 .iconSize = 16,
                 .width = BtnWidth::XL});
}

auto btnBackupDescriptor() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-backup-descriptor"),
                 .icon = icon::download,
                 .iconSize = 16,
                 .width = BtnWidth::XL});
}

auto btnRegisterDescriptor() -> Button * {
    return make({.role = ButtonRole::Default,
                 .label = TR("btn-register-descriptor"),
                 .icon = icon::usb,
                 .iconSize = 16,
                 .width = BtnWidth::XL});
}

auto btnRemoveRoute() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::trash,
                 .iconSize = 14,
                 .width = BtnWidth::Icon});
}

auto btnRemoveXpub() -> Button * {
    return make(
        {.role = ButtonRole::InlineIcon, .icon = icon::x, .iconSize = 14, .width = BtnWidth::Icon});
}

auto btnSaveLabel() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::check,
                 .iconSize = 15,
                 .tooltip = TR("btn-save"),
                 .width = BtnWidth::Icon});
}

auto btnVerifyOnDeviceSm() -> Button * {
    return make({.role = ButtonRole::InlineIcon,
                 .icon = icon::usb,
                 .iconSize = 15,
                 .width = BtnWidth::Icon});
}

auto btnCopyValue() -> CopyButton * {
    return makeCopy(CopyButton::Size::Small, 14, TR("common-copy"));
}

auto btnCopySm() -> CopyButton * {
    return makeCopy(CopyButton::Size::Small, 15, TR("common-copy"));
}

auto btnCopyAddress() -> CopyButton * {
    return makeCopy(CopyButton::Size::Large, 20, TR("btn-copy-address"));
}

auto btnAwaitingPayment(int count) -> Button * {
    return make({.role = ButtonRole::Inline,
                 .label = TR("btn-awaiting-payment").arg(count),
                 .width = BtnWidth::Auto});
}

auto btnOpenWallet(const QString &name) -> Button * {
    return make({.role = ButtonRole::TabOpen, .label = name, .width = BtnWidth::LaunchEntry});
}

auto btnCreateAccount() -> Button * {
    return make({.role = ButtonRole::TabCreate,
                 .label = TR("btn-create-account"),
                 .width = BtnWidth::LaunchAdd});
}

auto pillDeviceReady() -> Badge * {
    return makePill(Badge::Role::Success, TR("pill-ready"), PillWidth::M);
}

auto pillDeviceLocked() -> Badge * {
    return makePill(Badge::Role::Warning, TR("pill-locked"), PillWidth::M);
}

auto pillDeviceUnsupported() -> Badge * {
    return makePill(Badge::Role::Error, TR("pill-unsupported"), PillWidth::M);
}

auto pillKeyConnected() -> Badge * {
    return makePill(Badge::Role::Success, TR("pill-connected"), PillWidth::M);
}

auto pillKeyHot() -> Badge * {
    return makePill(Badge::Role::Success, TR("pill-hot-key"), PillWidth::M);
}

auto pillKeyOffline() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-offline"), PillWidth::M);
}

auto pillAccountRegistered() -> Badge * {
    return makePill(Badge::Role::Success, TR("pill-registered"), PillWidth::M);
}

auto pillSignerFirst() -> Badge * {
    return makePill(Badge::Role::Success, TR("pill-signs-first"), PillWidth::L);
}

auto pillSignerReady() -> Badge * {
    return makePill(Badge::Role::Success, TR("pill-ready"), PillWidth::L);
}

auto pillSignerSigned() -> Badge * {
    auto *pill = makePill(Badge::Role::Success, TR("pill-signed"), PillWidth::L);
    pill->setIcon(icon::check(IconColor::Success));
    return pill;
}

auto pillMusigNeeded() -> Badge * {
    return makePill(Badge::Role::Warning, TR("pill-needed"), PillWidth::L);
}

auto pillMusigNonceAdded() -> Badge * {
    auto *pill = makePill(Badge::Role::Success, TR("pill-nonce"), PillWidth::L);
    pill->setIcon(icon::check(IconColor::Success));
    return pill;
}

auto pillMusigOnDevice() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-on-device"), PillWidth::L);
}

auto pillSignerWaiting() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-waiting"), PillWidth::L);
}

auto pillSignerDisconnected() -> Badge * {
    return makePill(Badge::Role::Warning, TR("pill-not-connected"), PillWidth::L);
}

auto pillSignerNoHot() -> Badge * {
    return makePill(Badge::Role::Warning, TR("pill-no-hot-secret"), PillWidth::L);
}

auto pillSignerNoRoute() -> Badge * {
    return makePill(Badge::Role::Error, TR("pill-no-usable-route"), PillWidth::L);
}

auto pillSignerUnregistered() -> Badge * {
    return makePill(Badge::Role::Error, TR("pill-not-registered"), PillWidth::L);
}

auto pillSignerNotInPath() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-not-in-path"), PillWidth::L);
}

auto pillSignerUnsupported() -> Badge * {
    return makePill(Badge::Role::Warning, TR("pill-unsupported"), PillWidth::L);
}

auto pillSignerNotInWallet() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-not-in-wallet"), PillWidth::L);
}

auto pillSdStepDone() -> Badge * {
    return makePill(Badge::Role::Success, TR("pill-done"), PillWidth::M);
}

auto pillTxSigned() -> Badge * {
    auto *pill = makePill(Badge::Role::Success, TR("pill-signed"), PillWidth::M);
    pill->setIcon(icon::check(IconColor::Success));
    return pill;
}

auto pillXpubUsed() -> Badge * {
    return makePill(Badge::Role::Success, TR("pill-used"), PillWidth::M);
}

auto pillXpubUnused() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-not-used"), PillWidth::M);
}

auto pillXpubFetched() -> Badge * {
    auto *pill = makePill(Badge::Role::Success, TR("pill-fetched"), PillWidth::M);
    pill->setIcon(icon::check(IconColor::Success));
    return pill;
}

auto pillXpubMissing() -> Badge * {
    return makePill(Badge::Role::Warning, TR("pill-missing"), PillWidth::M);
}

auto pillRegRegistered() -> Badge * {
    auto *pill = makePill(Badge::Role::Success, TR("pill-registered"), PillWidth::L);
    pill->setIcon(icon::check(IconColor::Success));
    return pill;
}

auto pillRegPending() -> Badge * {
    return makePill(Badge::Role::Warning, TR("pill-not-registered"), PillWidth::L);
}

auto pillRegRegistering() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-registering"), PillWidth::L);
}

auto pillRegChecking() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-checking"), PillWidth::L);
}

auto pillRegFailed() -> Badge * {
    return makePill(Badge::Role::Error, TR("pill-failed"), PillWidth::L);
}

auto pillRegNotNeeded() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-not-needed"), PillWidth::L);
}

auto pillAddrVerified() -> Badge * {
    auto *pill = makePill(Badge::Role::Success, TR("pill-verified"), PillWidth::M);
    pill->setIcon(icon::check(IconColor::Success));
    return pill;
}

auto pillAddrUnchecked() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-not-checked"), PillWidth::M);
}

auto pillAddrMismatch() -> Badge * {
    return makePill(Badge::Role::Error, TR("pill-mismatch"), PillWidth::M);
}

auto pillAddrNoScreen() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-no-screen"), PillWidth::M);
}

auto pillPluginUnloading() -> Badge * {
    return makePill(Badge::Role::Off, TR("pill-unloading"), PillWidth::L);
}

auto pillNetwork(const QString &network) -> Badge * {
    return makePill(Badge::Role::Neutral, network, PillWidth::Auto);
}

} // namespace catalog
