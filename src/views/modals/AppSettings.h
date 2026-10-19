#pragma once

#include "catalog/containers/Modal.h"
#include "interfaces/host.h"
#include "interfaces/types.h"
#include "theme/Palette.h"
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

class QEvent;

namespace catalog {
class Button;
class ComboBox;
class EmptyState;
class FieldGroup;
class FieldNote;
class Label;
class ModalStatus;
class Notice;
class ScrollArea;
class Section;
class SelectRow;
class Separator;
class ViewSlots;
} // namespace catalog

class PluginRegistry;
class IThemeProvider;

namespace modal {

class AppSettingsFlow;

// The global Settings: General, Plugin Manager and Extension Settings on the main
// step; disabling a plugin and a module's settings section are flows embedded in its
// place.
class AppSettings : public catalog::Modal {
    Q_OBJECT

public:
    AppSettings();
    void done(int result) override;

public slots:
    void onThemeSelectionChanged(int index);
    void onLanguageSelectionChanged(int index);
    void onScanClicked();
    void onPluginToggled(bool enabled);
    void onPluginAccessClicked();
    void onPluginsChanged();
    void onOpenSectionClicked();
    void onHostSettingsSectionsChanged();
    void onHostInstanceChanged(const QString &id);
    void onThemeNames(ReqId req_id, const QStringList &themes);
    void onThemePalette(ReqId req_id, theme::Palette palette);
    void onFlowViewChanged(const QString &title, ModalWidth width);
    void onFlowBack();
    void onFlowConfirmed();

protected:
    void init();
    void doConnect();
    void view();

    // Main view
    // Main view
    void initMain();
    void connectMain();
    auto viewMain() -> catalog::ScrollArea *;
    auto buildGeneral() -> QWidget *;
    auto buildPlugins() -> QWidget *;
    void showMain();
    auto pluginRow(const QString &id, const QString &meta) -> catalog::SelectRow *;
    void setScanning(bool scanning);
    void rebuildSettingSectionRows();
    [[nodiscard]] auto sectionModuleName(Host::SectionId section) const -> QString;
    void refreshThemeProviders();
    void requestThemeList();
    void applyPersistedTheme();
    void setThemePickerIndexSilently(int index);
    void updateThemeNote();
    void switchToLightTheme();

    // Shared
    // Shared
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    void openFlow(AppSettingsFlow *flow);
    void closeFlow();

private:
    catalog::ScrollArea *m_main = nullptr;

    catalog::Section *m_general = nullptr;
    catalog::FieldGroup *m_theme_field = nullptr;
    catalog::FieldGroup *m_language_field = nullptr;
    catalog::ComboBox *m_theme_picker = nullptr;
    catalog::ComboBox *m_language_picker = nullptr;
    catalog::FieldNote *m_theme_note = nullptr;
    catalog::FieldNote *m_language_note = nullptr;

    catalog::Section *m_plugins = nullptr;
    catalog::Button *m_scan_btn = nullptr;
    catalog::Button *m_scanning_btn = nullptr;
    catalog::Notice *m_registry_notice = nullptr;
    catalog::Notice *m_plugin_notice = nullptr;
    catalog::Label *m_scan_result = nullptr;
    qontrol::Column *m_plugins_column = nullptr;
    catalog::EmptyState *m_plugins_empty = nullptr;
    catalog::ModalStatus *m_scanning = nullptr;

    catalog::Separator *m_extensions_separator = nullptr;
    catalog::Section *m_extensions = nullptr;
    qontrol::Column *m_section_column = nullptr;

    PluginRegistry *m_plugin_registry = nullptr;
    Host *m_host = nullptr;
    HostEvents *m_host_events = nullptr;
    QHash<QString, QString> m_plugin_meta;
    QString m_disabling_id;
    QString m_unloading_id;
    bool m_scanning_plugins = false;

    QList<IThemeProvider *> m_theme_providers;
    QHash<IThemeProvider *, QString> m_provider_plugin;
    QHash<ReqId, IThemeProvider *> m_pending_theme_list;
    QHash<ReqId, IThemeProvider *> m_pending_palette;
    QHash<QString, IThemeProvider *> m_provider_for_theme_name;
    QHash<QString, QString> m_theme_plugin;
    bool m_updating_theme_picker = false;
    QString m_pending_theme_to_apply;

    // The flow picked off the main view, deleted once it goes back or the window closes.
    AppSettingsFlow *m_flow = nullptr;
};

// One page off the main settings, embedded in AppSettings in place of the main view:
// its views, their button bars and the work behind them. Deleting it stops that work.
class AppSettingsFlow : public QWidget {
    Q_OBJECT

public:
    explicit AppSettingsFlow(QWidget *parent = nullptr);
    // Shows the first view, once AppSettings listens to the signals.
    virtual void start() = 0;

signals:
    // The window title and width of the view now shown.
    void viewChanged(const QString &title, ModalWidth width);
    // Back left the flow: AppSettings shows its main view again.
    void back();
    // The flow's action is confirmed: AppSettings carries it out.
    void confirmed();

protected:
    [[nodiscard]] auto viewSlots() const -> catalog::ViewSlots *;

private:
    catalog::ViewSlots *m_slots = nullptr;
};

} // namespace modal
