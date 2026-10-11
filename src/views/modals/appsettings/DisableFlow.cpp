#include "DisableFlow.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <QStringList>
#include <Qontrol>
#include <common.h>

#include <utility>

namespace modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::Notice;

DisableFlow::DisableFlow(PluginRegistry *registry, QString id, QString meta,
                         QHash<QString, QString> theme_plugin)
    : m_plugin_registry(registry),
      m_id(std::move(id)),
      m_meta(std::move(meta)),
      m_theme_plugin(std::move(theme_plugin)) {
    init();
    doConnect();
    view();
}

void DisableFlow::start() {
    showDisable();
}

void DisableFlow::init() {
    initDisable();
}

void DisableFlow::doConnect() {
    connectDisable();
}

void DisableFlow::view() {
    m_disable = viewDisable();
}

// ===== Disable view =====

void DisableFlow::initDisable() {
    m_disable_question = new Label(LabelRole::Body);
    m_disable_question->setWordWrap(true);
    m_disable_accounts = new Notice;
    m_disable_themes = new Notice;
    m_disable_actions = new catalog::ModalActions;
    m_disable_actions->setBack(catalog::ModalActions::Back::Back);
    m_disable_btn = catalog::btnDisablePlugin();
    m_disable_actions->addAction(m_disable_btn);
}

void DisableFlow::connectDisable() {
    connect(m_disable_btn, &QPushButton::clicked, this, &DisableFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_disable_actions, &catalog::ModalActions::backClicked, this,
            &DisableFlow::onBackClicked, qontrol::UNIQUE);
}

auto DisableFlow::viewDisable() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_disable_question)
        ->push(m_disable_accounts)
        ->push(m_disable_themes);
}

void DisableFlow::showDisable() {
    QString name = m_plugin_registry->pluginName(m_id);
    m_disable_question->setText(TR("settings-disable-question").arg(name, m_meta));

    QStringList accounts = m_plugin_registry->pluginAccountNames(m_id);
    if (accounts.isEmpty()) {
        m_disable_accounts->setTone(Notice::Tone::Info);
        m_disable_accounts->setTitle(QString());
        m_disable_accounts->setText(TR("settings-disable-no-account"));
    } else if (accounts.size() == 1) {
        m_disable_accounts->setTone(Notice::Tone::Warning);
        m_disable_accounts->setTitle(TR("settings-disable-account-one"));
        m_disable_accounts->setText(TR("settings-disable-account-one-text").arg(accounts.first()));
    } else {
        m_disable_accounts->setTone(Notice::Tone::Warning);
        m_disable_accounts->setTitle(TR("settings-disable-accounts").arg(accounts.size()));
        m_disable_accounts->setText(
            TR("settings-disable-accounts-text").arg(accounts.join(QStringLiteral(", "))));
    }

    QStringList themes;
    for (auto it = m_theme_plugin.constBegin(); it != m_theme_plugin.constEnd(); ++it) {
        if (it.value() == m_id) {
            themes.append(it.key());
        }
    }
    themes.sort();
    m_disable_themes->setTone(Notice::Tone::Info);
    m_disable_themes->setText(TR("settings-disable-themes").arg(themes.join(QStringLiteral(", "))));
    m_disable_themes->setVisible(!themes.isEmpty());

    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_disable);
    viewSlots()->setFooter(m_disable_actions);
    emit viewChanged(TR("settings-disable-title"), ModalWidth::Wide);
}

// ===== Slots =====

void DisableFlow::onBackClicked() {
    emit back();
}

void DisableFlow::onNextClicked() {
    emit confirmed();
}

} // namespace modal
