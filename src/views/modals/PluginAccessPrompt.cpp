#include "PluginAccessPrompt.h"

#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"

#include <Qontrol>
#include <common.h>
#include <utility>

namespace modal {

namespace {
// The hash as the access page shows it: enough to tell two versions apart.
constexpr int SHORT_HASH = 16;
} // namespace

PluginAccessPrompt::PluginAccessPrompt(QString name, QString plugin_id, QString version,
                                       QString hash, plugin::Access asked)
    : m_name(std::move(name)),
      m_plugin_id(std::move(plugin_id)),
      m_version(std::move(version)),
      m_hash(std::move(hash)),
      m_asked(asked) {
    init();
    doConnect();
    view();
}

void PluginAccessPrompt::init() {
    m_intro = new catalog::Label;
    m_intro->setWordWrap(true);
    m_account_row = new catalog::SelectRow;
    m_account_row->setIcon(icon::coins);
    m_account_row->setChevron(false);
    m_account_row->setClickable(false);
    m_account_row->setVisible(m_asked.account);
    m_keys_row = new catalog::SelectRow;
    m_keys_row->setIcon(icon::key);
    m_keys_row->setChevron(false);
    m_keys_row->setClickable(false);
    m_keys_row->setVisible(m_asked.keys);
    m_state_row = new catalog::SelectRow;
    m_state_row->setIcon(icon::folder);
    m_state_row->setChevron(false);
    m_state_row->setClickable(false);
    m_state_row->setVisible(m_asked.state);
    m_library = new catalog::Label;
    m_library->setRole(catalog::LabelRole::Caption);
    m_library->setTone(catalog::LabelTone::Muted);
    m_note = new catalog::Label;
    m_note->setRole(catalog::LabelRole::Caption);
    m_note->setTone(catalog::LabelTone::Secondary);
    m_note->setWordWrap(true);
    m_deny_btn = catalog::btnDenyAccess();
    m_allow_btn = catalog::btnAllowAccess();
}

void PluginAccessPrompt::doConnect() {
    connect(m_allow_btn, &QPushButton::clicked, this, &PluginAccessPrompt::onAllowClicked,
            qontrol::UNIQUE);
    connect(m_deny_btn, &QPushButton::clicked, this, &PluginAccessPrompt::onDenyClicked,
            qontrol::UNIQUE);
}

void PluginAccessPrompt::onAllowClicked() {
    m_answered = true;
    m_allowed = true;
    accept();
}

void PluginAccessPrompt::onDenyClicked() {
    m_answered = true;
    m_allowed = false;
    reject();
}

void PluginAccessPrompt::view() {
    auto *actions = new catalog::ModalActions;
    actions->addAction(m_deny_btn);
    actions->addAction(m_allow_btn);
    auto *body = (new qontrol::Column)
                     ->spacing(resolve(Spacing::M))
                     ->push(m_intro)
                     ->push((new qontrol::Column)
                                ->spacing(resolve(Spacing::S))
                                ->push(m_account_row)
                                ->push(m_keys_row)
                                ->push(m_state_row))
                     ->push((new qontrol::Column)
                                ->spacing(resolve(Spacing::XS))
                                ->push(m_library)
                                ->push(m_note));
    setBody(body);
    setFooter(actions);
    setWidth(ModalWidth::S);
    retranslateUi();
}

void PluginAccessPrompt::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void PluginAccessPrompt::retranslateUi() {
    setWindowTitle(TR("plugin-access-prompt-title"));
    m_intro->setText(TR("plugin-access-prompt-intro").arg(m_name, m_plugin_id, m_version));
    m_account_row->setTitle(TR("plugin-access-accounts"));
    m_account_row->setSub(TR("plugin-access-accounts-sub"));
    m_keys_row->setTitle(TR("plugin-access-keys"));
    m_keys_row->setSub(TR("plugin-access-keys-sub"));
    m_state_row->setTitle(TR("plugin-access-state"));
    m_state_row->setSub(TR("plugin-access-state-sub"));
    m_library->setText(TR("plugin-access-library").arg(m_hash.left(SHORT_HASH)));
    m_note->setText(TR("plugin-access-prompt-note"));
    m_deny_btn->setText(TR("btn-deny-access"));
    m_allow_btn->setText(TR("btn-allow-access"));
    adjustSize();
}

} // namespace modal
