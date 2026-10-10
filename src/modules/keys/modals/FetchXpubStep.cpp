#include "FetchXpubStep.h"

#include "modules/keys/KeysModule.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

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

FetchXpubStep::FetchXpubStep(QWidget *parent) : QWidget(parent) {
    init();
    doConnect();
    view();
}

void FetchXpubStep::init() {
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
    m_template = (new qontrol::Row)
                     ->spacing(resolve(Spacing::XXS))
                     ->push(m_prefix)
                     ->push(m_account)
                     ->push(m_suffix)
                     ->push(m_script)
                     ->pushSpacer();

    m_custom = new catalog::Input(catalog::InputRole::Mono);
    m_custom->setWidth(Size::L);
    m_custom->setPlaceholderText(QStringLiteral("m/84'/%1'/0'").arg(KeysModule::coinType()));

    m_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_error->setTitle(TR("keys-fetch-failed-title"));
    m_error->setVisible(false);

    m_fetch = catalog::btnFetchThisXpub();
    m_actions = new catalog::ModalActions;
    // The design leaves Back out; a step that can return to the key gets one.
    m_actions->setBack(catalog::ModalActions::Back::Back);
    m_actions->addAction(m_fetch);
}

void FetchXpubStep::doConnect() {
    connect(m_purpose, &QComboBox::activated, this, &FetchXpubStep::onPurposeChanged,
            qontrol::UNIQUE);
    connect(m_account, &QLineEdit::textEdited, this, &FetchXpubStep::onAccountEdited,
            qontrol::UNIQUE);
    connect(m_script, &QComboBox::activated, this, &FetchXpubStep::onInputChanged, qontrol::UNIQUE);
    connect(m_custom, &QLineEdit::textEdited, this, &FetchXpubStep::onInputChanged,
            qontrol::UNIQUE);
    connect(m_fetch, &QPushButton::clicked, this, &FetchXpubStep::onFetchClicked, qontrol::UNIQUE);
    connect(m_actions, &catalog::ModalActions::backClicked, this, &FetchXpubStep::backClicked,
            qontrol::UNIQUE);
}

void FetchXpubStep::view() {
    m_fields = new catalog::FieldGroup;
    m_fields->addField(TR("keys-path-type"), m_purpose)
        ->addField(TR("keys-path"), m_template)
        ->addField(TR("keys-path"), m_custom);
    (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_fields)->push(m_error)->into(this);
}

void FetchXpubStep::reset(const QStringList &cached) {
    m_cached = cached;
    m_typed = false;
    m_purpose->setCurrentIndex(0);
    m_script->setCurrentIndex(0);
    m_custom->clear();
    m_account->setText(QString::number(freeAccount(purpose())));
    m_error->setVisible(false);
    refresh();
}

void FetchXpubStep::showError(const QString &error) {
    m_error->setText(error);
    m_error->setVisible(true);
}

auto FetchXpubStep::actions() const -> catalog::ModalActions * {
    return m_actions;
}

void FetchXpubStep::onPurposeChanged() {
    if (!m_typed && purpose() != PURPOSE_CUSTOM) {
        m_account->setText(QString::number(freeAccount(purpose())));
    }
    onInputChanged();
}

void FetchXpubStep::onAccountEdited() {
    m_typed = true;
    onInputChanged();
}

void FetchXpubStep::onInputChanged() {
    m_error->setVisible(false);
    refresh();
}

void FetchXpubStep::onFetchClicked() {
    if (!reason().isEmpty()) {
        return;
    }
    m_error->setVisible(false);
    emit fetchClicked(path());
}

auto FetchXpubStep::purpose() const -> QString {
    return m_purpose->currentData().toString();
}

auto FetchXpubStep::pathFor(const QString &purpose, int account) const -> QString {
    QString path =
        QStringLiteral("m/%1'/%2'/%3'").arg(purpose).arg(KeysModule::coinType()).arg(account);
    if (purpose == PURPOSE_MULTISIG) {
        path += QStringLiteral("/%1'").arg(m_script->currentData().toInt());
    }
    return path;
}

auto FetchXpubStep::path() const -> QString {
    if (purpose() == PURPOSE_CUSTOM) {
        return m_custom->text().trimmed();
    }
    bool ok = false;
    int account = m_account->text().toInt(&ok);
    return ok ? pathFor(purpose(), account) : QString();
}

auto FetchXpubStep::freeAccount(const QString &purpose) const -> int {
    if (purpose == PURPOSE_CUSTOM) {
        return 0;
    }
    int account = 0;
    while (account < MAX_ACCOUNT && m_cached.contains(pathFor(purpose, account))) {
        ++account;
    }
    return account;
}

auto FetchXpubStep::reason() const -> QString {
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

void FetchXpubStep::refresh() {
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

} // namespace keys::modal
