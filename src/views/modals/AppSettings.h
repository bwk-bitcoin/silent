#pragma once

#include "catalog/containers/Modal.h"
#include "interfaces/host.h"
#include "interfaces/types.h"
#include "theme/Palette.h"
#include <QHash>
#include <QList>
#include <QPointer>
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
class ModalActions;
class ModalStatus;
class Notice;
class ScrollArea;
class Section;
class SelectRow;
class Separator;
} // namespace catalog

class PluginRegistry;
class IThemeProvider;

namespace modal {

// The global Settings: General, Plugin Manager and Extension Settings on the main
// step; disabling a plugin and a module's settings section replace it in place.
class AppSettings : public catalog::Modal {
    Q_OBJECT

public:
    AppSettings();
    ~AppSettings() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onThemeSelectionChanged(int index);
    void onLanguageSelectionChanged(int index);
    void onScanClicked();
    void onPluginToggled(bool enabled);
    void onPluginsChanged();
    void onOpenSectionClicked();
    void onHostSettingsSectionsChanged();
    void onHostInstanceChanged(const QString &id);
    void onThemeNames(ReqId req_id, const QStringList &themes);
    void onThemePalette(ReqId req_id, theme::Palette palette);

protected:
    // The view on display: Back and the primary action act on it.
    enum class View { Main, Disable, Section };

    void init();
    void doConnect();
    void view();

    // Main view
    void initMain();
    void connectMain();
    auto viewMain() -> catalog::ScrollArea *;
    auto buildGeneral() -> QWidget *;
    auto buildPlugins() -> QWidget *;
    void showMain();
    void rebuildPluginRows();
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

    // Disable view
    void initDisable();
    void connectDisable();
    auto viewDisable() -> QWidget *;
    void showDisable(const QString &id);
    void disablePlugin();

    // Section view
    void initSection();
    void connectSection();
    auto viewSection() -> catalog::ScrollArea *;
    void showSection(const QString &title);
    void closeSection();

    // Shared
    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    View m_view = View::Main;
    catalog::ScrollArea *m_main = nullptr;
    QWidget *m_disable = nullptr;
    catalog::ScrollArea *m_section = nullptr;

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

    catalog::Label *m_disable_question = nullptr;
    catalog::Notice *m_disable_accounts = nullptr;
    catalog::Notice *m_disable_themes = nullptr;
    catalog::ModalActions *m_disable_actions = nullptr;
    catalog::Button *m_disable_btn = nullptr;

    catalog::Label *m_section_text = nullptr;
    qontrol::Column *m_section_slot = nullptr;
    catalog::ModalActions *m_section_actions = nullptr;
    catalog::Button *m_done_btn = nullptr;
    // The module owns its section widget: it is only lent to the slot while open.
    QPointer<QWidget> m_open_section;
    Host::SectionId m_open_section_id = 0;

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
};

} // namespace modal
