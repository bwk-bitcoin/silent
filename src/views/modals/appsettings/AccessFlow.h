#pragma once

#include <QString>

#include "views/modals/AppSettings.h"

namespace catalog {
class Button;
class Label;
class ModalActions;
} // namespace catalog

class PluginRegistry;

namespace modal {

// What an external plugin may use, one row per library version answered: its
// Accounts and Keys grants, changed at once, and Forget, after which the plugin is
// asked again on its next use.
class AccessFlow final : public AppSettingsFlow {
    Q_OBJECT

public:
    AccessFlow(PluginRegistry *registry, QString id);
    void start() override;

public slots:
    void onBackClicked();
    void onGrantToggled(bool allowed);
    void onForgetClicked();

protected:
    void init();
    void doConnect();
    void view();

    // Access view
    void initAccess();
    void connectAccess();
    [[nodiscard]] auto viewAccess() -> QWidget *;
    void showAccess();
    void rebuildRows();

private:
    PluginRegistry *m_plugin_registry = nullptr;
    QString m_id;

    QWidget *m_access = nullptr;
    catalog::Label *m_access_text = nullptr;
    qontrol::Column *m_rows = nullptr;
    catalog::Label *m_empty = nullptr;
    catalog::ModalActions *m_access_actions = nullptr;
    catalog::Button *m_done_btn = nullptr;
};

} // namespace modal
