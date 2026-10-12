#include "FetchXpub.h"

#include "modules/keys/DeviceName.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/KeysView.h"
#include "modules/keys/modals/Unseal.h"
#include "modules/keys/modals/keydetail/KeyRecord.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "theme/Palette.h"

#include <QDebug>
#include <QIntValidator>
#include <QRegularExpression>
#include <Qontrol>

#include <common.h>

namespace keys::modal {

namespace {

const QString PURPOSE_CUSTOM = QStringLiteral("custom");
const QString PURPOSE_MULTISIG = QStringLiteral("48");
constexpr int MAX_ACCOUNT = 99;
// BIP48's script type, its last level. 3' (Taproot) is not in BIP48 but is the
// convention wallets use for P2TR multisig.
constexpr int SCRIPT_P2WSH = 2;
constexpr int SCRIPT_P2SH_P2WSH = 1;
constexpr int SCRIPT_P2TR = 3;

const QRegularExpression CUSTOM_PATH_RE(QStringLiteral(R"(^m(/\d+'?)+$)"));

} // namespace

FetchXpub::FetchXpub(KeysModule *module, const QString &fingerprint)
    : m_module(module),
      m_fingerprint(fingerprint) {
    init();
    doConnect();
    view();
}

void FetchXpub::init() {
    for (const auto &entry :
         m_module->rust()->key_xpubs(rust::String(m_fingerprint.toStdString()))) {
        m_cached.append(xpubPath(KeysModule::toQt(entry.path)));
    }
    initFetch();
    initFetching();
}

void FetchXpub::doConnect() {
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &FetchXpub::onInstanceRemoved, qontrol::UNIQUE);
    connect(m_module, &KeysModule::hotXpubFetched, this, &FetchXpub::onHotXpubFetched,
            qontrol::UNIQUE);
    connect(m_module->bus(), &IKeys::xpubReady, this, &FetchXpub::onQrXpub, qontrol::UNIQUE);
    connect(m_module->bus(), &IKeys::error, this, &FetchXpub::onQrError, qontrol::UNIQUE);
    connectFetch();
}

void FetchXpub::view() {
    m_fetch_step = viewFetch();
    hideFetching();
    m_account->setText(QString::number(freeAccount(purpose())));
    refresh();
    showFetch();
}

// ===== Fetch view =====

void FetchXpub::initFetch() {
    m_purpose = new catalog::ComboBox;
    m_purpose->addItem(TR("keys-purpose-84"), QStringLiteral("84"));
    m_purpose->addItem(TR("keys-purpose-86"), QStringLiteral("86"));
    m_purpose->addItem(TR("keys-purpose-49"), QStringLiteral("49"));
    m_purpose->addItem(TR("keys-purpose-48"), PURPOSE_MULTISIG);
    m_purpose->addItem(TR("keys-purpose-87"), QStringLiteral("87"));
    m_purpose->addItem(TR("keys-purpose-custom"), PURPOSE_CUSTOM);
    m_purpose->setWidth(Size::L);

    m_prefix = new catalog::Label(catalog::LabelRole::Mono);
    m_account = new catalog::Input(catalog::InputRole::Mono);
    m_account->setFixedWidth(metric::ACCOUNT_INDEX_INPUT_WIDTH);
    m_account->setMaxLength(2);
    m_account->setValidator(new QIntValidator(0, MAX_ACCOUNT, m_account));
    m_account->setToolTip(TR("keys-account-index-tip"));
    m_suffix = new catalog::Label(QStringLiteral("'"), catalog::LabelRole::Mono);
    m_script = new catalog::ComboBox;
    m_script->addItem(TR("keys-script-p2wsh"), SCRIPT_P2WSH);
    m_script->addItem(TR("keys-script-p2sh-p2wsh"), SCRIPT_P2SH_P2WSH);
    m_script->addItem(TR("keys-script-p2tr"), SCRIPT_P2TR);
    m_script->setWidth(Size::S);

    m_custom = new catalog::Input(catalog::InputRole::Mono);
    m_custom->setWidth(Size::L);
    m_custom->setPlaceholderText(QStringLiteral("m/84'/%1'/0'").arg(KeysModule::coinType()));

    m_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_error->setTitle(TR("keys-fetch-failed-title"));
    m_error->setVisible(false);

    m_fetch = catalog::btnFetchThisXpub();
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_fetch);
}

void FetchXpub::connectFetch() {
    connect(m_purpose, &QComboBox::activated, this, &FetchXpub::onPurposeChanged, qontrol::UNIQUE);
    connect(m_account, &QLineEdit::textEdited, this, &FetchXpub::onAccountEdited, qontrol::UNIQUE);
    connect(m_script, &QComboBox::activated, this, &FetchXpub::onInputChanged, qontrol::UNIQUE);
    connect(m_custom, &QLineEdit::textEdited, this, &FetchXpub::onInputChanged, qontrol::UNIQUE);
    connect(m_fetch, &QPushButton::clicked, this, &FetchXpub::onNextClicked, qontrol::UNIQUE);
}

auto FetchXpub::viewFetch() -> QWidget * {
    m_template = (new qontrol::Row)
                     ->spacing(resolve(Spacing::XXS))
                     ->push(m_prefix)
                     ->push(m_account)
                     ->push(m_suffix)
                     ->push(m_script)
                     ->pushSpacer();
    m_fields = new catalog::FieldGroup;
    m_fields->addField(TR("keys-path-type"), m_purpose)
        ->addField(TR("keys-path"), m_template)
        ->addField(TR("keys-path"), m_custom);
    return (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_fields)->push(m_error);
}

void FetchXpub::showFetch() {
    m_view = View::Fetch;
    setWindowTitle(TR("keys-fetch-title"));
    setHeader(nullptr);
    setBody(m_fetch_step);
    setFooter(m_actions);
    setWidth(ModalWidth::L);
}

auto FetchXpub::purpose() const -> QString {
    return m_purpose->currentData().toString();
}

auto FetchXpub::pathFor(const QString &purpose, int account) const -> QString {
    QString path =
        QStringLiteral("m/%1'/%2'/%3'").arg(purpose).arg(KeysModule::coinType()).arg(account);
    if (purpose == PURPOSE_MULTISIG) {
        path += QStringLiteral("/%1'").arg(m_script->currentData().toInt());
    }
    return path;
}

auto FetchXpub::path() const -> QString {
    if (purpose() == PURPOSE_CUSTOM) {
        return m_custom->text().trimmed();
    }
    bool ok = false;
    int account = m_account->text().toInt(&ok);
    return ok ? pathFor(purpose(), account) : QString();
}

auto FetchXpub::freeAccount(const QString &purpose) const -> int {
    if (purpose == PURPOSE_CUSTOM) {
        return 0;
    }
    int account = 0;
    while (account < MAX_ACCOUNT && m_cached.contains(pathFor(purpose, account))) {
        ++account;
    }
    return account;
}

auto FetchXpub::reason() const -> QString {
    bool custom = purpose() == PURPOSE_CUSTOM;
    QString current = path();
    if (!custom && current.isEmpty()) {
        return TR("keys-fetch-reason-account");
    }
    if (custom && current.isEmpty()) {
        return TR("keys-fetch-reason-path");
    }
    if (custom && !CUSTOM_PATH_RE.match(current).hasMatch()) {
        return TR("keys-fetch-reason-format").arg(KeysModule::coinType());
    }
    if (m_cached.contains(current)) {
        return TR("keys-fetch-reason-cached");
    }
    return QString();
}

void FetchXpub::refresh() {
    bool custom = purpose() == PURPOSE_CUSTOM;
    m_fields->setFieldVisible(m_template, !custom);
    m_fields->setFieldVisible(m_custom, custom);
    // The path with the account index as the only part to type.
    m_prefix->setText(QStringLiteral("m/%1'/%2'/").arg(purpose()).arg(KeysModule::coinType()));
    bool multisig = purpose() == PURPOSE_MULTISIG;
    m_suffix->setText(multisig ? QStringLiteral("'/") : QStringLiteral("'"));
    m_script->setVisible(multisig);
    QString why = reason();
    m_fetch->setEnabled(why.isEmpty());
    m_actions->setReason(why);
}

void FetchXpub::showError(const QString &error) {
    m_error->setText(error);
    m_error->setVisible(true);
    // Shown again so the window fits the error it now holds.
    showFetch();
}

void FetchXpub::fetch() {
    if (!reason().isEmpty()) {
        return;
    }
    auto device = readyDevice(m_fingerprint);
    if (!device.has_value()) {
        auto info = keyInfo(m_module, m_fingerprint);
        if (info.has_value() && info->has_hot_secret) {
            fetchHot();
            return;
        }
        if (info.has_value() && hasRoute(*info, KeyRouteKind::Qr)) {
            fetchQr();
            return;
        }
        showError(TR("keys-fetch-xpub-connect"));
        return;
    }
    m_error->setVisible(false);
    m_signer = device->signer;
    m_instance = device->instanceId;
    m_path = path();
    connect(device->signer, &ISigner::xpub, this, &FetchXpub::onDeviceXpub, qontrol::UNIQUE);
    connect(device->signer, &ISigner::error, this, &FetchXpub::onDeviceError, qontrol::UNIQUE);
    m_req = device->signer->getXpub(m_path);
    m_fetching_step->setSubtitle(TR("keys-fetching-sub").arg(deviceName(*device->signer)));
    showFetching();
}

// No device: the hot key's password in the unseal modal over this one, which derives the
// xpub under its spinner; cached, it closes this modal too.
void FetchXpub::fetchHot() {
    m_error->setVisible(false);
    m_path = path();
    m_hot_error.clear();
    auto *unseal = new Unseal(TR("keys-fetch-title"), QString(), catalog::btnGetXpubHot());
    unseal->setBusy(TR("keys-fetching-title"), QString());
    connect(unseal, &Unseal::passwordEntered, this, &FetchXpub::onHotPassword, qontrol::UNIQUE);
    m_unseal = unseal;
    bool fetched = unseal->exec() == QDialog::Accepted;
    delete unseal;
    m_hot_req = 0;
    if (fetched) {
        accept();
        return;
    }
    if (!m_hot_error.isEmpty()) {
        showError(m_hot_error);
    }
}

// No device, no hot secret: the key's QR route, its request code and the scan of the
// answer in the signing flow over this modal. The module caches the xpub before it
// answers; fetched, this modal closes too.
void FetchXpub::fetchQr() {
    m_error->setVisible(false);
    m_path = path();
    m_qr_fetched = false;
    m_qr_error.clear();
    m_qr_req = m_module->bus()->newRequest();
    // Returns once the flow has closed, its answer delivered.
    m_module->fetchXpub(m_qr_req, m_fingerprint, m_path);
    m_qr_req = 0;
    if (m_qr_fetched) {
        accept();
        return;
    }
    if (!m_qr_error.isEmpty()) {
        showError(m_qr_error);
    }
}

// ===== Fetching view =====

void FetchXpub::initFetching() {
    m_fetching_step = new catalog::ModalStatus;
    m_fetching_step->setSpinner();
    m_fetching_step->setTitle(TR("keys-fetching-title"));
    m_fetching_step->setPadding(resolve(Spacing::M), resolve(Spacing::M));
}

void FetchXpub::showFetching() {
    m_view = View::Fetching;
    setWindowTitle(TR("keys-fetch-title"));
    setHeader(nullptr);
    setBody(m_fetching_step);
    setFooter(nullptr);
    setWidth(ModalWidth::L);
}

void FetchXpub::hideFetching() {
    keep(m_fetching_step);
}

void FetchXpub::stopFetch() {
    // Only this fetch's own connections: the device keeps serving the key detail.
    if (!m_signer.isNull()) {
        disconnect(m_signer, &ISigner::xpub, this, &FetchXpub::onDeviceXpub);
        disconnect(m_signer, &ISigner::error, this, &FetchXpub::onDeviceError);
    }
    m_signer = nullptr;
    m_req = 0;
}

// ===== Slots =====

void FetchXpub::onNextClicked() {
    switch (m_view) {
    case View::Fetch:
        fetch();
        return;
    case View::Fetching:
        return;
    }
}

void FetchXpub::onPurposeChanged() {
    if (!m_typed && purpose() != PURPOSE_CUSTOM) {
        m_account->setText(QString::number(freeAccount(purpose())));
    }
    onInputChanged();
}

void FetchXpub::onAccountEdited() {
    m_typed = true;
    onInputChanged();
}

void FetchXpub::onInputChanged() {
    m_error->setVisible(false);
    refresh();
}

void FetchXpub::onDeviceXpub(ReqId req_id, const QString &xpub) {
    if (m_req == 0 || req_id != m_req) {
        return;
    }
    QString instance = m_instance;
    QString path = m_path;
    stopFetch();
    auto result = m_module->cacheXpub(m_fingerprint, path, xpub);
    if (!result.is_ok) {
        showError(KeysModule::toQt(result.error));
        return;
    }
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && entry.instanceId == instance) {
            auto recorded = m_module->recordDevice(entry);
            if (!recorded.is_ok) {
                qWarning() << "keys:" << KeysModule::toQt(recorded.error);
            }
        }
    }
    accept();
}

void FetchXpub::onDeviceError(std::optional<ReqId> req_id, const QString &message) {
    if (m_req == 0 || !req_id.has_value() || req_id.value() != m_req) {
        return;
    }
    stopFetch();
    showError(message);
}

void FetchXpub::onHotPassword(const QString &password) {
    m_hot_req = m_module->nextReqId();
    m_module->fetchHotXpub(m_hot_req, m_fingerprint, password, m_path);
}

void FetchXpub::onHotXpubFetched(ReqId req_id, bool is_ok, const QString &xpub,
                                 const QString &error, bool wrong_password) {
    if (m_hot_req == 0 || req_id != m_hot_req || m_unseal.isNull()) {
        return;
    }
    m_hot_req = 0;
    if (wrong_password) {
        m_unseal->retry(TR("flow-password-wrong"));
        return;
    }
    if (is_ok) {
        auto cached = m_module->cacheXpub(m_fingerprint, m_path, xpub);
        if (!cached.is_ok) {
            m_hot_error = KeysModule::toQt(cached.error);
        }
        m_unseal->finish(cached.is_ok);
        return;
    }
    m_hot_error = error;
    m_unseal->finish(false);
}

void FetchXpub::onQrXpub(ReqId req_id, [[maybe_unused]] const QString &xpub,
                         [[maybe_unused]] const QString &fingerprint,
                         [[maybe_unused]] const QString &path) {
    if (m_qr_req == 0 || req_id != m_qr_req) {
        return;
    }
    m_qr_fetched = true;
}

void FetchXpub::onQrError(std::optional<ReqId> req_id, const QString &message) {
    if (m_qr_req == 0 || !req_id.has_value() || req_id.value() != m_qr_req) {
        return;
    }
    m_qr_error = message;
}

void FetchXpub::onInstanceRemoved(const QString &id) {
    if (m_req == 0 || id != m_instance) {
        return;
    }
    stopFetch();
    showError(TR("keys-register-disconnected"));
}

} // namespace keys::modal
