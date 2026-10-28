#include "PluginAccessPrompt.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Spinner.h"
#include "host/PluginAccess.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"

#include <Qontrol>
#include <common.h>
#include <string>
#include <utility>

namespace modal {

namespace {
// The hash as the access page shows it: enough to tell two versions apart.
constexpr int SHORT_HASH = 16;
} // namespace

PluginAccessPrompt::PluginAccessPrompt(QString name, QString version, PluginAccess answers,
                                       uint64_t epoch, uint64_t grant_version, plugin::Access asked)
    : m_name(std::move(name)),
      m_version(std::move(version)),
      m_answers(std::move(answers)),
      m_epoch(epoch),
      m_grant_version(grant_version),
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
    m_saving = new catalog::Spinner(metric::ROW_SPINNER_SIZE);
    m_saving->setVisible(false);
    m_deny_btn = catalog::btnDenyAccess();
    m_allow_btn = catalog::btnAllowAccess();
}

void PluginAccessPrompt::doConnect() {
    connect(m_allow_btn, &QPushButton::clicked, this, &PluginAccessPrompt::onAllowClicked,
            qontrol::UNIQUE);
    connect(m_deny_btn, &QPushButton::clicked, this, &PluginAccessPrompt::onDenyClicked,
            qontrol::UNIQUE);
}

void PluginAccessPrompt::view() {
    auto *actions = new catalog::ModalActions;
    actions->addAction(m_deny_btn);
    actions->addAction(m_allow_btn);
    auto *body =
        (new qontrol::Column)
            ->spacing(resolve(Spacing::M))
            ->push(m_intro)
            ->push((new qontrol::Column)
                       ->spacing(resolve(Spacing::S))
                       ->push(m_account_row)
                       ->push(m_keys_row)
                       ->push(m_state_row))
            ->push(
                (new qontrol::Column)->spacing(resolve(Spacing::XS))->push(m_library)->push(m_note))
            ->push(m_saving);
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
    m_intro->setText(
        TR("plugin-access-prompt-intro")
            .arg(m_name, QString::fromStdString(std::string(m_answers.plugin_id)), m_version));
    m_account_row->setTitle(TR("plugin-access-accounts"));
    m_account_row->setSub(TR("plugin-access-accounts-sub"));
    m_keys_row->setTitle(TR("plugin-access-keys"));
    m_keys_row->setSub(TR("plugin-access-keys-sub"));
    m_state_row->setTitle(TR("plugin-access-state"));
    m_state_row->setSub(TR("plugin-access-state-sub"));
    m_library->setText(
        TR("plugin-access-library")
            .arg(QString::fromStdString(std::string(m_answers.hash)).left(SHORT_HASH)));
    m_note->setText(TR("plugin-access-prompt-note"));
    m_deny_btn->setText(TR("btn-deny-access"));
    m_allow_btn->setText(TR("btn-allow-access"));
    adjustSize();
}

// The prompt's only save: Allow and Deny stay disabled while it runs. The job owns
// copies, so closing the prompt meanwhile leaves it to finish alone.
void PluginAccessPrompt::answer(bool allowed) {
    if (m_answered) {
        return;
    }
    m_answered = true;
    m_allowed = allowed;
    m_allow_btn->setEnabled(false);
    m_deny_btn->setEnabled(false);
    m_saving->setVisible(true);
    auto *job = new pluginaccess::Job;
    auto *registry = AppController::get()->pluginRegistry();
    if (registry != nullptr) {
        connect(job, &pluginaccess::Job::saved, registry, &PluginRegistry::onPluginAccessSaved,
                qontrol::UNIQUE);
    }
    connect(job, &pluginaccess::Job::saved, this, &PluginAccessPrompt::onSaved, qontrol::UNIQUE);
    job->saveIfCurrent(0, withAnswer(), m_epoch, m_grant_version);
}

// The answers read, the grants asked set to the answer clicked.
auto PluginAccessPrompt::withAnswer() const -> PluginAccess {
    auto grant = m_allowed ? PluginGrant::Allowed : PluginGrant::Denied;
    auto answers = m_answers;
    if (m_asked.account) {
        answers.account = grant;
    }
    if (m_asked.keys) {
        answers.keys = grant;
    }
    if (m_asked.state) {
        answers.state = grant;
    }
    return answers;
}

// ===== Slots =====

void PluginAccessPrompt::onAllowClicked() {
    answer(true);
}

void PluginAccessPrompt::onDenyClicked() {
    answer(false);
}

// A failed save closes the prompt with the error: the host shows it and refuses the request.
void PluginAccessPrompt::onSaved([[maybe_unused]] uint64_t revision, const QString &error) {
    m_saving->setVisible(false);
    m_saved = error.isEmpty();
    m_error = error;
    if (m_saved) {
        m_answers = withAnswer();
        accept();
    } else {
        reject();
    }
}

} // namespace modal
