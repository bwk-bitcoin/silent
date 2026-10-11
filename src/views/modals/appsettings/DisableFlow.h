#pragma once

#include <QHash>
#include <QString>

#include "views/modals/AppSettings.h"

namespace catalog {
class Button;
class Label;
class ModalActions;
class Notice;
} // namespace catalog

class PluginRegistry;

namespace modal {

// Asks to confirm disabling a plugin, naming the accounts that use it and the themes
// it provides.
class DisableFlow final : public AppSettingsFlow {
    Q_OBJECT

public:
    // `meta` is the plugin's id and version line; `theme_plugin` maps each theme to the
    // plugin providing it.
    DisableFlow(PluginRegistry *registry, QString id, QString meta,
                QHash<QString, QString> theme_plugin);
    // Asks to confirm.
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();

protected:
    void init();
    void doConnect();
    void view();

    // Disable view
    void initDisable();
    void connectDisable();
    [[nodiscard]] auto viewDisable() -> QWidget *;
    void showDisable();

private:
    PluginRegistry *m_plugin_registry = nullptr;
    QString m_id;
    QString m_meta;
    QHash<QString, QString> m_theme_plugin;

    QWidget *m_disable = nullptr;
    catalog::Label *m_disable_question = nullptr;
    catalog::Notice *m_disable_accounts = nullptr;
    catalog::Notice *m_disable_themes = nullptr;
    catalog::ModalActions *m_disable_actions = nullptr;
    catalog::Button *m_disable_btn = nullptr;
};

} // namespace modal
