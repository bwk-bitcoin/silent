#pragma once

#include <QString>
#include <qevent.h>

#include "catalog/containers/Modal.h"
#include "interfaces/types.h"

namespace catalog {
class Button;
class Label;
class SelectRow;
} // namespace catalog

namespace modal {

// Asked on an external plugin's first use of the accounts, the keys or its saved state, or when it
// asks through Host::requestAccess(): Allow or Deny, kept for this plugin library. Closing it
// answers nothing, so the plugin is asked again next time.
class PluginAccessPrompt : public catalog::Modal {
    Q_OBJECT

public:
    // `asked` holds the grants asked, the rows shown.
    PluginAccessPrompt(QString name, QString plugin_id, QString version, QString hash,
                       plugin::Access asked);

    // Allow or Deny was clicked.
    [[nodiscard]] auto answered() const -> bool {
        return m_answered;
    }

    [[nodiscard]] auto allowed() const -> bool {
        return m_allowed;
    }

public slots:
    void onAllowClicked();
    void onDenyClicked();

protected:
    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    QString m_name;
    QString m_plugin_id;
    QString m_version;
    QString m_hash;
    plugin::Access m_asked;
    bool m_answered = false;
    bool m_allowed = false;
    catalog::Label *m_intro = nullptr;
    catalog::SelectRow *m_account_row = nullptr;
    catalog::SelectRow *m_keys_row = nullptr;
    catalog::SelectRow *m_state_row = nullptr;
    catalog::Label *m_library = nullptr;
    catalog::Label *m_note = nullptr;
    catalog::Button *m_deny_btn = nullptr;
    catalog::Button *m_allow_btn = nullptr;
};

} // namespace modal
