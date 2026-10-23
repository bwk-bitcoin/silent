#pragma once

#include <QHash>
#include <QString>
#include <cstdint>
#include <silent.h>

#include "views/modals/AppSettings.h"

namespace catalog {
class Button;
class Label;
class ModalActions;
class Notice;
class SelectRow;
} // namespace catalog

class PluginRegistry;

namespace modal {

// What an external plugin may use, one row per library version answered: its
// Accounts and Keys grants, changed at once, and Forget, after which the plugin is
// asked again on its next use. The answers are read with the plugin's storage epoch
// when the view opens; a change refused because the plugin was removed since, or any
// other failed read or save, shows the error and disables the rows until Try Again
// reads them anew.
class AccessFlow final : public AppSettingsFlow {
    Q_OBJECT

public:
    AccessFlow(PluginRegistry *registry, QString id);
    void start() override;

public slots:
    void onBackClicked();
    void onGrantClicked(bool allowed);
    void onForgetClicked();
    void onReloadClicked();

protected:
    void init();
    void doConnect();
    void view();

    // Access view
    void initAccess();
    void connectAccess();
    [[nodiscard]] auto viewAccess() -> QWidget *;
    void showAccess();
    void loadRows();
    [[nodiscard]] auto rowSub(const PluginAccess &entry) const -> QString;
    void showFailure(const QString &error);

private:
    PluginRegistry *m_plugin_registry = nullptr;
    QString m_id;
    // The plugin's storage epoch read with the rows: every change is saved against it.
    uint64_t m_epoch = 0;
    // The answers each row shows, and the row, by library hash.
    QHash<QString, PluginAccess> m_entries;
    QHash<QString, catalog::SelectRow *> m_entry_rows;

    QWidget *m_access = nullptr;
    catalog::Label *m_access_text = nullptr;
    catalog::Notice *m_notice = nullptr;
    catalog::Button *m_reload_btn = nullptr;
    qontrol::Column *m_rows = nullptr;
    catalog::Label *m_empty = nullptr;
    catalog::ModalActions *m_access_actions = nullptr;
    catalog::Button *m_done_btn = nullptr;
};

} // namespace modal
