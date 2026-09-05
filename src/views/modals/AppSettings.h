#pragma once

#include "interfaces/types.h"
#include "theme/Palette.h"
#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <Qontrol>
#include <qevent.h>

namespace catalog {
class Button;
class ComboBox;
class Label;
class Toggle;
}

class PluginRegistry;
class Host;
class HostEvents;
class IThemeProvider;

namespace modal {

class AppSettings : public qontrol::Modal {
    Q_OBJECT

public:
    explicit AppSettings(QWidget *parent = nullptr);

public slots:
    void onPluginToggled(bool enabled);
    void onThemeSelectionChanged(int index);
    void onLanguageSelectionChanged(int index);
    void onScanPluginsClicked();
    void onThemeNames(ReqId req_id, const QStringList &themes);
    void onThemePalette(ReqId req_id, theme::Palette palette);
    void onHostSettingsSectionsChanged();
    void onOpenSectionClicked();
    void onHostInstanceChanged(const QString &id);

protected:
    void init();
    void doConnect();
    void view();
    void rebuildPluginRows();
    void rebuildSettingSectionRows();
    void refreshThemeProviders();
    void requestThemeList();
    void applyPersistedTheme();
    void setThemePickerIndexSilently(int index);
    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    catalog::Label *m_title = nullptr;
    catalog::Label *m_general_title = nullptr;
    catalog::Label *m_theme_label = nullptr;
    catalog::Label *m_language_label = nullptr;
    catalog::Label *m_plugin_manager_title = nullptr;
    catalog::Label *m_extension_settings_title = nullptr;
    catalog::Button *m_scan_plugins_btn = nullptr;
    catalog::ComboBox *m_theme_picker = nullptr;
    catalog::ComboBox *m_language_picker = nullptr;
    qontrol::Column *m_plugins_column = nullptr;
    qontrol::Column *m_section_column = nullptr;
    QWidget *m_extension_section = nullptr;
    PluginRegistry *m_plugin_registry = nullptr;
    Host *m_host = nullptr;
    HostEvents *m_host_events = nullptr;
    QList<QWidget *> m_plugin_rows;
    QList<QWidget *> m_setting_section_rows;
    QHash<catalog::Toggle *, QString> m_toggle_plugin_ids;
    QList<IThemeProvider *> m_theme_providers;
    QHash<ReqId, IThemeProvider *> m_pending_theme_list;
    QHash<ReqId, IThemeProvider *> m_pending_palette;
    QHash<QString, IThemeProvider *> m_provider_for_theme_name;
    bool m_updating_theme_picker = false;
    bool m_updating_language_picker = false;
    QString m_pending_theme_to_apply;
};

} // namespace modal
