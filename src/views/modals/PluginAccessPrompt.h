#pragma once

#include <QString>
#include <cstdint>
#include <qevent.h>
#include <silent.h>

#include "catalog/containers/Modal.h"
#include "interfaces/types.h"

namespace catalog {
class Button;
class Label;
class SelectRow;
class Spinner;
} // namespace catalog

namespace modal {

// Asked on an external plugin's first use of the accounts, the keys or its saved state, or when it
// asks through Host::requestAccess(): Allow or Deny, kept for this plugin library. The answer is
// saved off the GUI thread against the storage epoch and persisted grant version it was read with.
// The prompt closes once the save answers. Closing it answers nothing, so the plugin is asked again
// next time.
class PluginAccessPrompt : public catalog::Modal {
    Q_OBJECT

public:
    // `answers` holds the answers read with the storage `epoch` and `grant_version`,
    // `asked` the grants asked, the rows shown.
    PluginAccessPrompt(QString name, QString version, PluginAccess answers, uint64_t epoch,
                       uint64_t grant_version, plugin::Access asked);

    // Allow or Deny was clicked.
    [[nodiscard]] auto answered() const -> bool {
        return m_answered;
    }

    [[nodiscard]] auto allowed() const -> bool {
        return m_allowed;
    }

    // The answer was saved: answers() holds it.
    [[nodiscard]] auto saved() const -> bool {
        return m_saved;
    }

    // The answers saved, or the answers read when none was.
    [[nodiscard]] auto answers() const -> PluginAccess {
        return m_answers;
    }

    // Why the answer was not saved, empty unless the save failed.
    [[nodiscard]] auto error() const -> QString {
        return m_error;
    }

public slots:
    void onAllowClicked();
    void onDenyClicked();
    void onSaved(uint64_t revision, const QString &error);

protected:
    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    void answer(bool allowed);
    [[nodiscard]] auto withAnswer() const -> PluginAccess;

private:
    QString m_name;
    QString m_version;
    PluginAccess m_answers;
    uint64_t m_epoch = 0;
    uint64_t m_grant_version = 0;
    plugin::Access m_asked;
    bool m_answered = false;
    bool m_allowed = false;
    bool m_saved = false;
    QString m_error;
    catalog::Label *m_intro = nullptr;
    catalog::SelectRow *m_account_row = nullptr;
    catalog::SelectRow *m_keys_row = nullptr;
    catalog::SelectRow *m_state_row = nullptr;
    catalog::Label *m_library = nullptr;
    catalog::Label *m_note = nullptr;
    catalog::Spinner *m_saving = nullptr;
    catalog::Button *m_deny_btn = nullptr;
    catalog::Button *m_allow_btn = nullptr;
};

} // namespace modal
