#include "AddKey.h"

#include "modules/keys/KeysModule.h"

#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Label.h"
#include "catalog/form/FormRow.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/inputs/Input.h"
#include "catalog/inputs/TextEdit.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "views/utils.h"

#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

#include <common.h>

using catalog::Button;
using catalog::ButtonRole;
using catalog::FormRow;
using catalog::Input;
using catalog::InputRole;
using catalog::Label;
using catalog::LabelRole;
using catalog::TextEdit;
using catalog::ValidatedInput;
using catalog::ValidationMark;

namespace keys::modal {

namespace {

const char *const K_ADD_KEY_MODE = "addKeyMode";

const QRegularExpression ORIGIN_RE(QStringLiteral(R"(^\[?([0-9a-fA-F]{8})((?:/\d+['h]?)*)\]?$)"));

auto isXpubValid(const QString &text) -> bool {
    static const QStringList prefixes = {QStringLiteral("xpub"), QStringLiteral("tpub"),
                                         QStringLiteral("vpub"), QStringLiteral("zpub"),
                                         QStringLiteral("upub"), QStringLiteral("ypub")};
    if (text.size() < 100) {
        return false;
    }
    return std::ranges::any_of(prefixes,
                               [&text](const QString &prefix) { return text.startsWith(prefix); });
}

auto networkName(Network network) -> QString {
    switch (network) {
    case Network::Bitcoin:
        return QStringLiteral("bitcoin");
    case Network::Testnet:
        return QStringLiteral("testnet");
    case Network::Signet:
        return QStringLiteral("signet");
    case Network::Regtest:
        return QStringLiteral("regtest");
    default:
        return QString();
    }
}

auto routeFromString(const QString &text) -> KeyRouteKind {
    if (text == QStringLiteral("hot")) {
        return KeyRouteKind::Hot;
    }
    if (text == QStringLiteral("usb")) {
        return KeyRouteKind::Usb;
    }
    if (text == QStringLiteral("qr")) {
        return KeyRouteKind::Qr;
    }
    if (text == QStringLiteral("sdcard")) {
        return KeyRouteKind::SdCard;
    }
    return KeyRouteKind::Unknown;
}

} // namespace

AddKey::AddKey(KeysModule *module, QWidget *parent) : m_module(module) {
    Q_UNUSED(parent);
    setWindowTitle(TR("keys-add-key-title"));
    init();
    doConnect();
    view();
    applyMode();
}

void AddKey::init() {
    auto *qrRow = new catalog::SelectRow;
    qrRow->setIcon(icon::qr(IconColor::Accent));
    qrRow->setTitle(TR("keys-mode-qr"));
    qrRow->setProperty(K_ADD_KEY_MODE, static_cast<int>(Mode::AirgapQr));

    auto *sdRow = new catalog::SelectRow;
    sdRow->setIcon(icon::sdCard(IconColor::Accent));
    sdRow->setTitle(TR("keys-mode-sd"));
    sdRow->setProperty(K_ADD_KEY_MODE, static_cast<int>(Mode::AirgapSd));

    auto *xpubRow = new catalog::SelectRow;
    xpubRow->setIcon(icon::key(IconColor::Accent));
    xpubRow->setTitle(TR("keys-mode-xpub"));
    xpubRow->setProperty(K_ADD_KEY_MODE, static_cast<int>(Mode::ImportXpub));

    auto *fileRow = new catalog::SelectRow;
    fileRow->setIcon(icon::folder(IconColor::Accent));
    fileRow->setTitle(TR("keys-mode-file"));
    fileRow->setProperty(K_ADD_KEY_MODE, static_cast<int>(Mode::ImportFile));

    m_mode_rows = {qrRow, sdRow, xpubRow, fileRow};

    m_alias_input = new Input;
    m_alias_input->setPlaceholderText(TR("keys-alias-placeholder"));

    m_origin_input = new ValidatedInput;
    m_origin_input->setWidth(Size::L);
    m_origin_input->input()->setPlaceholderText(QStringLiteral("[73c5da0a/84'/0'/0']"));

    m_xpub_input = new ValidatedInput(InputRole::Mono);
    m_xpub_input->setWidth(Size::L);

    m_qr_response = new TextEdit;
    m_qr_response->setPlaceholderText(TR("keys-qr-paste"));
    m_qr_response->setMaximumHeight(60);
    m_qr_response->setMinimumWidth(260);
    m_decode_qr_btn = new Button(TR("keys-qr-decode"));

    m_browse_btn = new Button(TR("keys-pick-file"));
    m_file_label = new Label(QString(), LabelRole::Caption);

    m_error_label = new Label(QString(), LabelRole::Caption);
    m_error_label->setVisible(false);

    m_register_btn = new Button(TR("keys-register-key"), ButtonRole::Primary);
    m_register_btn->setEnabled(false);
    m_cancel_btn = new Button(TR("common-cancel"));

    m_qr_row = (new qontrol::Row)
                   ->push(m_qr_response)
                   ->pushSpacer(resolve(Spacing::XS))
                   ->push(m_decode_qr_btn);
    m_file_row = (new qontrol::Row)
                     ->push(m_browse_btn)
                     ->pushSpacer(resolve(Spacing::XS))
                     ->push(m_file_label);

    m_origin_row = new FormRow(TR("keys-origin"), m_origin_input);
    m_xpub_row = new FormRow(TR("keys-xpub"), m_xpub_input);
}

void AddKey::doConnect() {
    for (auto *row : m_mode_rows) {
        connect(row, &catalog::SelectRow::clicked, this, &AddKey::onModeClicked, qontrol::UNIQUE);
    }
    connect(m_origin_input, &ValidatedInput::textChanged, this, &AddKey::onOriginChanged,
            qontrol::UNIQUE);
    connect(m_xpub_input, &ValidatedInput::textChanged, this, &AddKey::onXpubChanged,
            qontrol::UNIQUE);
    connect(m_decode_qr_btn, &QPushButton::clicked, this, &AddKey::onDecodeQrClicked,
            qontrol::UNIQUE);
    connect(m_browse_btn, &QPushButton::clicked, this, &AddKey::onBrowseFileClicked,
            qontrol::UNIQUE);
    connect(m_register_btn, &QPushButton::clicked, this, &AddKey::onRegisterClicked,
            qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &QDialog::reject, qontrol::UNIQUE);
}

void AddKey::view() {
    auto *col = (new qontrol::Column)
                    ->push(new Label(TR("keys-add-key-title"), LabelRole::Heading))
                    ->pushSpacer(resolve(Spacing::S));

    for (auto *row : m_mode_rows) {
        col->push(row)->pushSpacer(resolve(Spacing::XS));
    }

    col->pushSpacer(resolve(Spacing::M))
        ->push(new catalog::Separator)
        ->pushSpacer(resolve(Spacing::M))
        ->push(new FormRow(TR("keys-alias"), m_alias_input))
        ->push(m_origin_row)
        ->push(m_xpub_row)
        ->push(m_qr_row)
        ->push(m_file_row)
        ->push(m_error_label)
        ->pushSpacer(resolve(Spacing::M));

    auto *buttonRow = (new qontrol::Row)
                          ->pushSpacer()
                          ->push(m_cancel_btn)
                          ->pushSpacer(resolve(Spacing::XS))
                          ->push(m_register_btn);
    col->push(buttonRow);

    setMainWidget(margin(col));
}

void AddKey::applyMode() {
    bool isQr = (m_mode == Mode::AirgapQr);
    bool isFile = (m_mode == Mode::ImportFile);

    m_qr_row->setVisible(isQr);
    m_file_row->setVisible(isFile);
    m_origin_row->setVisible(!isFile);
    m_xpub_row->setVisible(!isFile);
    m_origin_input->input()->setReadOnly(isQr);
    m_xpub_input->input()->setReadOnly(isQr);

    updateRegisterState();
}

void AddKey::updateRegisterState() {
    showError(QString());

    if (m_mode == Mode::ImportFile) {
        m_register_btn->setEnabled(!m_file_path.isEmpty());
        return;
    }

    bool complete = originIsValid() && isXpubValid(m_xpub_input->input()->text().trimmed());
    if (!complete) {
        m_register_btn->setEnabled(false);
        return;
    }

    if (m_module != nullptr && m_module->hasKey(fingerprintFromOrigin())) {
        m_register_btn->setEnabled(false);
        showError(TR("keys-already-registered"));
        return;
    }

    m_register_btn->setEnabled(true);
}

void AddKey::showError(const QString &message) {
    m_error_label->setText(message);
    m_error_label->setVisible(!message.isEmpty());
}

auto AddKey::originIsValid() const -> bool {
    return ORIGIN_RE.match(m_origin_input->input()->text().trimmed()).hasMatch();
}

auto AddKey::fingerprintFromOrigin() const -> QString {
    auto match = ORIGIN_RE.match(m_origin_input->input()->text().trimmed());
    if (!match.hasMatch()) {
        return QString();
    }
    return match.captured(1).toLower();
}

auto AddKey::pathFromOrigin() const -> QString {
    auto match = ORIGIN_RE.match(m_origin_input->input()->text().trimmed());
    if (!match.hasMatch()) {
        return QStringLiteral("m");
    }
    return QStringLiteral("m") + match.captured(2);
}

void AddKey::onModeClicked() {
    auto *row = qobject_cast<QObject *>(sender());
    if (row == nullptr) {
        return;
    }
    m_mode = static_cast<Mode>(row->property(K_ADD_KEY_MODE).toInt());
    applyMode();
}

void AddKey::onOriginChanged() {
    QString text = m_origin_input->input()->text().trimmed();
    if (text.isEmpty()) {
        m_origin_input->setState(ValidationMark::State::None);
    } else {
        m_origin_input->setState(originIsValid() ? ValidationMark::State::Valid
                                                 : ValidationMark::State::Invalid);
    }
    updateRegisterState();
}

void AddKey::onXpubChanged() {
    QString text = m_xpub_input->input()->text().trimmed();
    if (text.isEmpty()) {
        m_xpub_input->setState(ValidationMark::State::None);
    } else {
        m_xpub_input->setState(isXpubValid(text) ? ValidationMark::State::Valid
                                                 : ValidationMark::State::Invalid);
    }
    updateRegisterState();
}

void AddKey::onDecodeQrClicked() {
    if (m_module == nullptr || m_module->rust() == nullptr) {
        return;
    }

    auto payload = m_qr_response->toPlainText().trimmed();
    auto response = m_module->rust()->qr_decode_response(rust::String(payload.toStdString()));
    if (!response->is_ok()) {
        showError(TR("keys-qr-failed").arg(KeysModule::toQt(response->get_error())));
        return;
    }
    if (response->kind() != QrResponseKind::Xpubs) {
        showError(TR("keys-qr-wrong-kind"));
        return;
    }

    // The bridge already parses the response into structured fields (a fingerprint
    // plus one KeyXpub per path) rather than one packed string, so the inputs are
    // filled straight from those fields instead of splitting a payload string.
    auto xpubs = response->xpubs();
    if (xpubs.empty()) {
        showError(TR("keys-qr-wrong-kind"));
        return;
    }
    const auto &entry = xpubs[0];
    QString fingerprint = KeysModule::toQt(response->fingerprint());
    QString path = KeysModule::toQt(entry.path);
    QString suffix = path.startsWith(QStringLiteral("m")) ? path.mid(1) : path;

    m_origin_input->input()->setText(QStringLiteral("[%1%2]").arg(fingerprint, suffix));
    m_xpub_input->input()->setText(KeysModule::toQt(entry.xpub));

    onOriginChanged();
    onXpubChanged();
}

void AddKey::onBrowseFileClicked() {
    QString path =
        QFileDialog::getOpenFileName(this, TR("keys-pick-file"), QString(), TR("keys-file-filter"));
    if (path.isEmpty()) {
        return;
    }
    m_file_path = path;
    m_file_label->setText(path);
    updateRegisterState();
}

void AddKey::onRegisterClicked() {
    if (m_module == nullptr || m_module->rust() == nullptr) {
        return;
    }

    if (m_mode == Mode::ImportFile) {
        registerFromFile();
        return;
    }

    QString fingerprint = fingerprintFromOrigin();
    if (fingerprint.isEmpty()) {
        return;
    }
    if (m_module->hasKey(fingerprint)) {
        showError(TR("keys-already-registered"));
        return;
    }
    QString alias = m_alias_input->text().trimmed();
    if (alias.isEmpty()) {
        alias = fingerprint;
    }
    QString xpub = m_xpub_input->input()->text().trimmed();
    QString path = pathFromOrigin();

    auto addResult = m_module->rust()->add_key(rust::String(fingerprint.toStdString()),
                                               rust::String(alias.toStdString()));
    if (!addResult.is_ok) {
        showError(TR("keys-add-failed").arg(KeysModule::toQt(addResult.error)));
        return;
    }

    // A key with no cached xpub is still a key: a failure here is reported but does
    // not undo the record just created.
    auto cacheResult = m_module->rust()->cache_xpub(rust::String(fingerprint.toStdString()),
                                                    rust::String(path.toStdString()),
                                                    rust::String(xpub.toStdString()));
    if (!cacheResult.is_ok) {
        showError(TR("keys-add-failed").arg(KeysModule::toQt(cacheResult.error)));
        return;
    }

    if (m_mode == Mode::AirgapQr || m_mode == Mode::AirgapSd) {
        KeyRouteKind kind = (m_mode == Mode::AirgapQr) ? KeyRouteKind::Qr : KeyRouteKind::SdCard;
        auto routeResult = m_module->addRoute(fingerprint, kind);
        if (!routeResult.is_ok) {
            showError(TR("keys-add-failed").arg(KeysModule::toQt(routeResult.error)));
            return;
        }
    }
    // ImportXpub adds no route: the key is held by a signer that is not connected
    // here, and a route is added from the key modal once one exists.

    emit keyAdded(fingerprint);
    accept();
}

void AddKey::registerFromFile() {
    QFile file(m_file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        showError(TR("keys-file-open-failed"));
        return;
    }

    QJsonParseError parseError{};
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        showError(TR("keys-file-invalid"));
        return;
    }

    QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("version")).toInt() > 1) {
        showError(TR("keys-file-version"));
        return;
    }
    if (obj.value(QStringLiteral("network")).toString() != networkName(KeysModule::network())) {
        showError(TR("keys-file-network"));
        return;
    }

    QString fingerprint = obj.value(QStringLiteral("fingerprint")).toString().trimmed().toLower();
    if (fingerprint.isEmpty()) {
        showError(TR("keys-file-missing-fingerprint"));
        return;
    }
    if (m_module->hasKey(fingerprint)) {
        showError(TR("keys-already-registered"));
        return;
    }

    QString alias = obj.value(QStringLiteral("alias")).toString();
    if (alias.isEmpty()) {
        alias = fingerprint;
    }

    auto addResult = m_module->rust()->add_key(rust::String(fingerprint.toStdString()),
                                               rust::String(alias.toStdString()));
    if (!addResult.is_ok) {
        showError(TR("keys-add-failed").arg(KeysModule::toQt(addResult.error)));
        return;
    }

    for (const auto &xpubValue : obj.value(QStringLiteral("xpubs")).toArray()) {
        QJsonObject xpubObj = xpubValue.toObject();
        QString path = xpubObj.value(QStringLiteral("path")).toString();
        QString xpub = xpubObj.value(QStringLiteral("xpub")).toString();
        auto cacheResult = m_module->rust()->cache_xpub(rust::String(fingerprint.toStdString()),
                                                        rust::String(path.toStdString()),
                                                        rust::String(xpub.toStdString()));
        if (!cacheResult.is_ok) {
            showError(TR("keys-add-failed").arg(KeysModule::toQt(cacheResult.error)));
            return;
        }
    }

    for (const auto &routeValue : obj.value(QStringLiteral("routes")).toArray()) {
        KeyRouteKind kind = routeFromString(routeValue.toString());
        if (kind == KeyRouteKind::Unknown) {
            showError(TR("keys-file-unknown-route"));
            return;
        }
        auto routeResult = m_module->addRoute(fingerprint, kind);
        if (!routeResult.is_ok) {
            showError(TR("keys-add-failed").arg(KeysModule::toQt(routeResult.error)));
            return;
        }
    }

    emit keyAdded(fingerprint);
    accept();
}

} // namespace keys::modal
