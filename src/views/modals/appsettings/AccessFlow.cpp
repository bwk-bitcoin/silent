#include "AccessFlow.h"

#include "catalog/Button.h"
#include "catalog/ClearRows.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/Toggle.h"
#include "host/PluginAccess.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QStringList>
#include <Qontrol>
#include <common.h>

#include <string>
#include <utility>

namespace modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

namespace {
constexpr const char *K_HASH = "accessHash";
constexpr const char *K_GRANT = "accessGrant";
constexpr const char *GRANT_ACCOUNT = "account";
constexpr const char *GRANT_KEYS = "keys";
// The hash as a row shows it: enough to tell two versions apart.
constexpr int SHORT_HASH = 16;

auto toQString(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}

auto grantToggle(PluginGrant grant, const QString &hash, const char *which) -> catalog::Toggle * {
    auto *toggle = new catalog::Toggle;
    toggle->setChecked(grant == PluginGrant::Allowed);
    toggle->setProperty(K_HASH, hash);
    toggle->setProperty(K_GRANT, QString::fromLatin1(which));
    return toggle;
}
} // namespace

AccessFlow::AccessFlow(PluginRegistry *registry, QString id)
    : m_plugin_registry(registry),
      m_id(std::move(id)) {
    init();
    doConnect();
    view();
}

void AccessFlow::start() {
    showAccess();
}

void AccessFlow::init() {
    initAccess();
}

void AccessFlow::doConnect() {
    connectAccess();
}

void AccessFlow::view() {
    m_access = viewAccess();
}

// ===== Access view =====

void AccessFlow::initAccess() {
    m_access_text = new Label(LabelRole::Body);
    m_access_text->setWordWrap(true);
    m_access_text->setTone(LabelTone::Secondary);
    m_rows = new qontrol::Column;
    m_rows->spacing(resolve(Spacing::S));
    m_empty = new Label(LabelRole::Caption);
    m_empty->setTone(LabelTone::Muted);
    m_empty->setWordWrap(true);
    m_access_actions = new catalog::ModalActions;
    m_access_actions->setBack(catalog::ModalActions::Back::AllSettings);
    m_done_btn = catalog::btnDone();
    m_access_actions->addAction(m_done_btn);
}

void AccessFlow::connectAccess() {
    connect(m_done_btn, &QPushButton::clicked, this, &AccessFlow::onBackClicked, qontrol::UNIQUE);
    connect(m_access_actions, &catalog::ModalActions::backClicked, this, &AccessFlow::onBackClicked,
            qontrol::UNIQUE);
}

auto AccessFlow::viewAccess() -> QWidget * {
    auto *column = (new qontrol::Column)
                       ->spacing(resolve(Spacing::M))
                       ->push(m_access_text)
                       ->push(m_rows)
                       ->push(m_empty);
    // The main view's width, whatever the rows hold.
    column->setFixedWidth(metric::SETTINGS_BODY_WIDTH);
    return column;
}

void AccessFlow::showAccess() {
    QString name = m_plugin_registry->pluginName(m_id);
    m_access_text->setText(TR("plugin-access-flow-text").arg(name));
    rebuildRows();
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_access);
    viewSlots()->setFooter(m_access_actions);
    emit viewChanged(TR("plugin-access-flow-title").arg(name), ModalWidth::Wide);
}

void AccessFlow::rebuildRows() {
    catalog::clearRows(m_rows);
    QString current = m_plugin_registry->pluginHash(m_id);
    auto entries = pluginaccess::forPlugin(m_id);
    for (const auto &entry : entries) {
        QString hash = toQString(entry.hash);

        auto *account = grantToggle(entry.account, hash, GRANT_ACCOUNT);
        auto *keys = grantToggle(entry.keys, hash, GRANT_KEYS);
        connect(account, &QCheckBox::toggled, this, &AccessFlow::onGrantToggled, qontrol::UNIQUE);
        connect(keys, &QCheckBox::toggled, this, &AccessFlow::onGrantToggled, qontrol::UNIQUE);
        auto *forget = catalog::btnForgetAccess();
        forget->setProperty(K_HASH, hash);
        connect(forget, &QPushButton::clicked, this, &AccessFlow::onForgetClicked, qontrol::UNIQUE);
        auto *accountLabel = new Label(LabelRole::Caption);
        accountLabel->setText(TR("plugin-access-accounts"));
        auto *keysLabel = new Label(LabelRole::Caption);
        keysLabel->setText(TR("plugin-access-keys"));
        auto *grants = (new qontrol::Row)
                           ->push(accountLabel)
                           ->pushSpacer(resolve(Spacing::XS))
                           ->push(account)
                           ->pushSpacer(resolve(Spacing::L))
                           ->push(keysLabel)
                           ->pushSpacer(resolve(Spacing::XS))
                           ->push(keys)
                           ->pushSpacer(resolve(Spacing::L))
                           ->push(forget);

        QStringList unasked;
        if (entry.account == PluginGrant::Unasked) {
            unasked.append(TR("plugin-access-accounts"));
        }
        if (entry.keys == PluginGrant::Unasked) {
            unasked.append(TR("plugin-access-keys"));
        }

        auto *row = new catalog::SelectRow;
        row->setIcon(icon::puzzle);
        row->setTitle(TR("plugin-access-library").arg(hash.left(SHORT_HASH)));
        QString sub = hash == current ? TR("plugin-access-current") : TR("plugin-access-earlier");
        if (!unasked.isEmpty()) {
            sub += QLatin1Char(' ') +
                   TR("plugin-access-unasked").arg(unasked.join(QStringLiteral(", ")));
        }
        row->setSub(sub);
        row->setTrailing(grants);
        row->setChevron(false);
        row->setClickable(false);
        m_rows->push(row);
        row->show();
    }
    m_empty->setText(TR("plugin-access-none"));
    m_empty->setVisible(entries.isEmpty());
}

// ===== Slots =====

void AccessFlow::onBackClicked() {
    emit back();
}

void AccessFlow::onGrantToggled(bool allowed) {
    auto *toggle = sender();
    if (toggle == nullptr) {
        return;
    }
    QString hash = toggle->property(K_HASH).toString();
    auto entry = pluginaccess::answers(m_id, hash);
    auto grant = allowed ? PluginGrant::Allowed : PluginGrant::Denied;
    if (toggle->property(K_GRANT).toString() == QLatin1String(GRANT_ACCOUNT)) {
        entry.account = grant;
    } else {
        entry.keys = grant;
    }
    ::app_set_plugin_access(entry);
    rebuildRows();
}

void AccessFlow::onForgetClicked() {
    auto *button = sender();
    if (button == nullptr) {
        return;
    }
    auto entry = pluginaccess::answers(m_id, button->property(K_HASH).toString());
    entry.account = PluginGrant::Unasked;
    entry.keys = PluginGrant::Unasked;
    ::app_set_plugin_access(entry);
    rebuildRows();
}

} // namespace modal
