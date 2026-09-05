#include "AppSettings.h"
#include "AppController.h"
#include "i18n/I18nManager.h"
#include "i18n/Tr.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "interfaces/instance.h"
#include "interfaces/theme.h"
#include "views/utils.h"
#include "catalog/Button.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/display/Label.h"
#include "catalog/containers/ScrollArea.h"
#include "theme/Theme.h"
#include "catalog/inputs/Toggle.h"
#include <QCheckBox>
#include <QVariant>
#include <QVBoxLayout>
#include <common.h>

namespace modal {

using catalog::Button;
using catalog::Label;
using catalog::LabelRole;
using catalog::ComboBox;
using catalog::Toggle;
using catalog::ToggleRole;

AppSettings::AppSettings([[maybe_unused]] QWidget *parent) : qontrol::Modal() {
    setWindowTitle(TR("settings-title"));
    resize(1040, 640);
    init();
    doConnect();
    view();
    rebuildPluginRows();
    rebuildSettingSectionRows();
    refreshThemeProviders();
    requestThemeList();
    applyPersistedTheme();
}

void AppSettings::init() {
    m_title = new Label(TR("settings-title"), LabelRole::Title);
    m_title->setAlignment(Qt::AlignCenter);
    m_general_title = new Label(TR("settings-general"), LabelRole::Section);
    m_theme_label = new Label(TR("settings-theme"), LabelRole::Body);
    m_theme_label->setFixedWidth(resolve(Size::S));
    m_language_label = new Label(TR("settings-language"), LabelRole::Body);
    m_language_label->setFixedWidth(resolve(Size::S));
    m_plugin_manager_title = new Label(TR("settings-plugin-manager"), LabelRole::Section);
    m_extension_settings_title = new Label(TR("settings-extension-settings"), LabelRole::Section);
    m_scan_plugins_btn = new Button(TR("settings-scan-plugins"));

    m_theme_picker = new ComboBox;
    m_theme_picker->setWidth(Size::L);
    m_theme_picker->setEnabled(false);

    m_language_picker = new ComboBox;
    m_language_picker->setWidth(Size::L);
    struct LanguageOption {
        const char *label;
        const char *code;
    };
    static constexpr LanguageOption languages[] = {
        {.label = "English", .code = "en"},   {.label = "Francais", .code = "fr"},
        {.label = "Italiano", .code = "it"},  {.label = "Deutsch", .code = "de"},
        {.label = "Portugues", .code = "pt"}, {.label = "Espanol", .code = "es"},
    };
    for (const auto &language : languages) {
        m_language_picker->addItem(QString::fromLatin1(language.label),
                                   QString::fromLatin1(language.code));
    }
    int localeIndex = m_language_picker->findData(i18n::I18nManager::get()->selectedLocale());
    if (localeIndex < 0) {
        localeIndex = m_language_picker->findData(QStringLiteral("en"));
    }
    if (localeIndex >= 0) {
        m_updating_language_picker = true;
        m_language_picker->setCurrentIndex(localeIndex);
        m_updating_language_picker = false;
    }

    m_plugins_column = new qontrol::Column;
    m_section_column = new qontrol::Column;

    auto *controller = AppController::get();
    if (controller != nullptr) {
        m_plugin_registry = controller->pluginRegistry();
    }

    m_host = Host::get();
    if (m_host != nullptr) {
        m_host_events = m_host->events();
    }
}

void AppSettings::doConnect() {
    connect(m_scan_plugins_btn, &QPushButton::clicked, this, &AppSettings::onScanPluginsClicked,
            qontrol::UNIQUE);
    if (m_plugin_registry != nullptr) {
        connect(m_plugin_registry, &PluginRegistry::enabledPluginsChanged, this,
                &AppSettings::rebuildPluginRows, qontrol::UNIQUE);
    }
    if (m_theme_picker != nullptr) {
        connect(m_theme_picker,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this,
                &AppSettings::onThemeSelectionChanged,
                qontrol::UNIQUE);
    }
    if (m_language_picker != nullptr) {
        connect(m_language_picker,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this,
                &AppSettings::onLanguageSelectionChanged,
                qontrol::UNIQUE);
    }
    if (m_host_events != nullptr) {
        connect(m_host_events, &HostEvents::settingSectionsChanged, this,
                &AppSettings::onHostSettingsSectionsChanged, qontrol::UNIQUE);
        connect(m_host_events, &HostEvents::instanceRegistered, this, &AppSettings::onHostInstanceChanged,
                qontrol::UNIQUE);
        connect(m_host_events, &HostEvents::instanceRemoved, this, &AppSettings::onHostInstanceChanged,
                qontrol::UNIQUE);
    }
}

void AppSettings::view() {
    auto *titleRow = (new qontrol::Row)->pushSpacer()->push(m_title)->pushSpacer();

    auto *themeRow = (new qontrol::Row)
                         ->push(m_theme_label)
                         ->pushSpacer(resolve(Spacing::S))
                         ->push(m_theme_picker)
                         ->pushSpacer();
    auto *languageRow = (new qontrol::Row)
                            ->push(m_language_label)
                            ->pushSpacer(resolve(Spacing::S))
                            ->push(m_language_picker)
                            ->pushSpacer();
    auto *pluginManagerRow = (new qontrol::Row)
                                 ->push(m_plugin_manager_title)
                                 ->pushSpacer()
                                 ->push(m_scan_plugins_btn);

    auto *content = (new qontrol::Column)
                        ->push(titleRow)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push((new qontrol::Row)->push(m_general_title)->pushSpacer())
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(themeRow)
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(languageRow)
                        ->pushSpacer(resolve(Spacing::M))
                        ->push(pluginManagerRow)
                        ->pushSpacer(resolve(Spacing::S))
                        ->push(m_plugins_column);

    m_extension_section = (new qontrol::Column)
                              ->push((new qontrol::Row)->push(m_extension_settings_title)->pushSpacer())
                              ->pushSpacer(resolve(Spacing::S))
                              ->push(m_section_column);
    content->pushSpacer(resolve(Spacing::M))->push(m_extension_section);

    auto *scroll = new catalog::ScrollArea;
    scroll->setWidget(margin(content));

    setMainWidget(scroll);
}

void AppSettings::rebuildPluginRows() {
    if (m_plugins_column == nullptr) {
        return;
    }

    m_plugins_column->clear();
    m_plugin_rows.clear();
    m_toggle_plugin_ids.clear();

    if (m_plugin_registry == nullptr) {
        auto *empty = new Label(TR("settings-registry-unavailable"), LabelRole::Caption);
        auto *emptyRow = (new qontrol::Row)->push(empty)->pushSpacer();
        m_plugin_rows.append(emptyRow);
        m_plugins_column->push(emptyRow);
        return;
    }

    auto plugins = m_plugin_registry->plugins();
    for (auto *plugin : plugins) {
        if (plugin == nullptr) {
            continue;
        }

        auto meta = plugin->meta();
        QString id = meta.value(QStringLiteral("id")).trimmed();
        if (id.isEmpty()) {
            continue;
        }
        // Static-linked builtins are not user-managed; only external plugins are listed.
        if (m_plugin_registry->pluginHasBuiltinModules(id)) {
            continue;
        }

        QString name = meta.value(QStringLiteral("name")).trimmed().isEmpty()
            ? id
            : meta.value(QStringLiteral("name")).trimmed();
        QString version = meta.value(QStringLiteral("version")).trimmed();

        auto *nameLabel = new Label(name, LabelRole::Body);
        QString subtitle = id;
        if (!version.isEmpty()) {
            subtitle += QStringLiteral("  v") + version;
        }
        auto *metaLabel = new Label(subtitle, LabelRole::Caption);

        auto *labelCol = (new qontrol::Column)->push(nameLabel)->push(metaLabel);
        auto *toggle = new Toggle(ToggleRole::Default);
        toggle->setChecked(m_plugin_registry->isPluginEnabled(id));
        toggle->setProperty("pluginId", id);
        m_toggle_plugin_ids.insert(toggle, id);
        connect(toggle, &QCheckBox::toggled, this, &AppSettings::onPluginToggled, qontrol::UNIQUE);

        auto *row = (new qontrol::Row)
                        ->push(labelCol)
                        ->pushSpacer()
                        ->push(toggle);
        m_plugin_rows.append(row);
        m_plugins_column->push(row);
    }
}

void AppSettings::rebuildSettingSectionRows() {
    if (m_section_column == nullptr) {
        return;
    }

    m_section_column->clear();
    m_setting_section_rows.clear();

    if (m_host == nullptr) {
        if (m_extension_section != nullptr) {
            m_extension_section->setVisible(false);
        }
        return;
    }

    auto sections = m_host->settingSections();
    if (m_extension_section != nullptr) {
        m_extension_section->setVisible(!sections.isEmpty());
    }
    if (sections.isEmpty()) {
        return;
    }

    for (const auto &entry : sections) {
        if (entry.content == nullptr) {
            continue;
        }

        QString title = entry.title.trimmed().isEmpty()
            ? TR("settings-section-fallback").arg(entry.id)
            : entry.title.trimmed();

        // The widget is shown only inside the modal the button opens, never parented
        // here: a section that lives in this dialog dies with it, and the module owns it.
        auto *openBtn = new Button(TR("settings-section-open"));
        openBtn->setProperty("sectionId", QVariant::fromValue(entry.id));
        connect(openBtn, &QPushButton::clicked, this, &AppSettings::onOpenSectionClicked,
                qontrol::UNIQUE);

        auto *row = (new qontrol::Row)
                        ->push(new Label(title, LabelRole::Body))
                        ->pushSpacer()
                        ->push(openBtn);

        m_setting_section_rows.append(row);
        m_section_column->push(row);
        m_section_column->pushSpacer(resolve(Spacing::S));
    }
}

void AppSettings::onOpenSectionClicked() {
    auto *button = qobject_cast<QPushButton *>(sender());
    if (button == nullptr || m_host == nullptr) {
        return;
    }
    auto sectionId = button->property("sectionId").value<Host::SectionId>();

    // Re-query rather than capturing the widget: a module can drop its section while
    // the settings dialog is open.
    QWidget *content = nullptr;
    QString title;
    for (const auto &entry : m_host->settingSections()) {
        if (entry.id == sectionId && entry.content != nullptr) {
            content = entry.content;
            title = entry.title.trimmed();
            break;
        }
    }
    if (content == nullptr) {
        return;
    }

    auto *modal = new qontrol::Modal;
    modal->setWindowTitle(title.isEmpty() ? TR("settings-section-fallback").arg(sectionId) : title);
    auto *closeBtn = new Button(TR("common-close"));
    connect(closeBtn, &QPushButton::clicked, modal, &QDialog::accept, qontrol::UNIQUE);

    auto *column = (new qontrol::Column)
                       ->push(content)
                       ->pushSpacer(resolve(Spacing::M))
                       ->push((new qontrol::Row)->pushSpacer()->push(closeBtn));
    modal->setMainWidget(margin(column));
    modal->exec();

    // setMainWidget reparented the section into the modal; take it back before the
    // modal is destroyed or the module's widget goes with it.
    content->setParent(nullptr);
    modal->deleteLater();
}

void AppSettings::refreshThemeProviders() {
    m_theme_providers.clear();
    m_pending_theme_list.clear();
    m_pending_palette.clear();
    m_provider_for_theme_name.clear();

    if (m_host == nullptr) {
        return;
    }

    auto instances = m_host->instances();
    for (auto *instance : instances) {
        if (instance == nullptr) {
            continue;
        }
        auto *provider = instance->theme();
        if (provider == nullptr || !provider->implemented()) {
            continue;
        }
        if (!m_theme_providers.contains(provider)) {
            m_theme_providers.append(provider);
            connect(provider, qOverload<ReqId, QStringList>(&IThemeProvider::themes), this,
                    &AppSettings::onThemeNames, qontrol::UNIQUE);
            connect(provider, qOverload<ReqId, theme::Palette>(&IThemeProvider::palette), this,
                    &AppSettings::onThemePalette, qontrol::UNIQUE);
        }
    }
}

void AppSettings::requestThemeList() {
    if (m_theme_picker == nullptr) {
        return;
    }

    m_updating_theme_picker = true;
    m_theme_picker->clear();
    m_theme_picker->addItem(TR("settings-theme-light"), QStringLiteral("light"));
    m_theme_picker->addItem(TR("settings-theme-dark"), QStringLiteral("dark"));
    m_updating_theme_picker = false;

    for (auto *provider : m_theme_providers) {
        if (provider == nullptr) {
            continue;
        }
        ReqId reqId = provider->themes();
        if (reqId == 0) {
            continue;
        }
        m_pending_theme_list.insert(reqId, provider);
    }

    m_theme_picker->setEnabled(true);
}

void AppSettings::applyPersistedTheme() {
    QString activeTheme = QString::fromStdString(std::string(app_active_theme().c_str())).trimmed();
    if (activeTheme.isEmpty()) {
        return;
    }

    if (activeTheme == QStringLiteral("light") || activeTheme == QStringLiteral("dark")) {
        if (m_theme_picker != nullptr) {
            int index = m_theme_picker->findData(activeTheme);
            if (index >= 0) {
                setThemePickerIndexSilently(index);
            }
        }
        if (auto *theme = Theme::get(); theme != nullptr) {
            theme->setMode(activeTheme == QStringLiteral("dark") ? ThemeMode::Dark : ThemeMode::Light);
        }
        return;
    }

    m_pending_theme_to_apply = activeTheme;
    if (m_theme_picker != nullptr) {
        int index = m_theme_picker->findData(activeTheme);
        if (index >= 0) {
            setThemePickerIndexSilently(index);
        }
    }

    if (m_provider_for_theme_name.contains(activeTheme)) {
        auto *provider = m_provider_for_theme_name.value(activeTheme);
        if (provider != nullptr) {
            ReqId reqId = provider->palette(activeTheme);
            if (reqId != 0) {
                m_pending_palette.insert(reqId, provider);
            }
        }
    }
}

void AppSettings::setThemePickerIndexSilently(int index) {
    if (m_theme_picker == nullptr) {
        return;
    }
    m_updating_theme_picker = true;
    m_theme_picker->setCurrentIndex(index);
    m_updating_theme_picker = false;
}

void AppSettings::onPluginToggled(bool enabled) {
    auto *toggle = qobject_cast<Toggle *>(sender());
    if (toggle == nullptr || m_plugin_registry == nullptr) {
        return;
    }

    QString id = m_toggle_plugin_ids.value(toggle);
    if (id.isEmpty()) {
        return;
    }

    m_plugin_registry->setEnabled(id, enabled);
}

void AppSettings::onThemeSelectionChanged(int index) {
    if (m_updating_theme_picker || m_theme_picker == nullptr || index < 0) {
        return;
    }

    QString themeName = m_theme_picker->itemData(index).toString().trimmed();
    if (themeName.isEmpty()) {
        return;
    }

    if (themeName == QStringLiteral("light") || themeName == QStringLiteral("dark")) {
        if (auto *theme = Theme::get(); theme != nullptr) {
            theme->setMode(themeName == QStringLiteral("dark") ? ThemeMode::Dark : ThemeMode::Light);
        }
        app_set_active_theme(rust::String(themeName.toStdString()));
        return;
    }

    if (!m_provider_for_theme_name.contains(themeName)) {
        return;
    }
    auto *provider = m_provider_for_theme_name.value(themeName);
    if (provider == nullptr) {
        return;
    }

    app_set_active_theme(rust::String(themeName.toStdString()));
    ReqId reqId = provider->palette(themeName);
    if (reqId != 0) {
        m_pending_palette.insert(reqId, provider);
    }
}

void AppSettings::onLanguageSelectionChanged(int index) {
    if (m_updating_language_picker || m_language_picker == nullptr || index < 0) {
        return;
    }
    QString locale = m_language_picker->itemData(index).toString().trimmed();
    if (locale.isEmpty()) {
        return;
    }
    static_cast<void>(i18n::I18nManager::get()->applyLocale(locale, true));
}

void AppSettings::onScanPluginsClicked() {
    if (m_plugin_registry != nullptr) {
        m_plugin_registry->rescanDiscoveredPlugins();
    }
}

void AppSettings::onThemeNames(ReqId req_id, const QStringList &themes) {
    if (!m_pending_theme_list.contains(req_id) || m_theme_picker == nullptr) {
        return;
    }

    auto *provider = m_pending_theme_list.take(req_id);
    if (provider == nullptr) {
        return;
    }

    for (const auto &themeNameRaw : themes) {
        QString themeName = themeNameRaw.trimmed();
        if (themeName.isEmpty()) {
            continue;
        }
        if (!m_provider_for_theme_name.contains(themeName)) {
            m_provider_for_theme_name.insert(themeName, provider);
        }
        if (m_theme_picker->findData(themeName) < 0) {
            m_updating_theme_picker = true;
            m_theme_picker->addItem(themeName, themeName);
            m_updating_theme_picker = false;
        }
    }

    if (!m_pending_theme_to_apply.isEmpty()) {
        int activeIndex = m_theme_picker->findData(m_pending_theme_to_apply);
        if (activeIndex >= 0) {
            setThemePickerIndexSilently(activeIndex);
            auto *paletteProvider = m_provider_for_theme_name.value(m_pending_theme_to_apply, nullptr);
            if (paletteProvider != nullptr) {
                ReqId paletteReqId = paletteProvider->palette(m_pending_theme_to_apply);
                if (paletteReqId != 0) {
                    m_pending_palette.insert(paletteReqId, paletteProvider);
                }
            }
            m_pending_theme_to_apply.clear();
        }
    }
}

void AppSettings::onThemePalette(ReqId req_id, theme::Palette palette) {
    if (!m_pending_palette.contains(req_id)) {
        return;
    }
    m_pending_palette.remove(req_id);

    auto *theme = Theme::get();
    if (theme == nullptr) {
        return;
    }
    theme->setPalette(palette);
    theme->apply();
    if (window() != nullptr) {
        window()->update();
        window()->repaint();
    }
}

void AppSettings::onHostSettingsSectionsChanged() {
    rebuildSettingSectionRows();
}

void AppSettings::onHostInstanceChanged(const QString &id) {
    Q_UNUSED(id);
    refreshThemeProviders();
    requestThemeList();
    applyPersistedTheme();
}

void AppSettings::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    qontrol::Modal::changeEvent(event);
}

void AppSettings::retranslateUi() {
    setWindowTitle(TR("settings-title"));
    m_title->setText(TR("settings-title"));
    m_general_title->setText(TR("settings-general"));
    m_theme_label->setText(TR("settings-theme"));
    m_language_label->setText(TR("settings-language"));
    m_plugin_manager_title->setText(TR("settings-plugin-manager"));
    m_extension_settings_title->setText(TR("settings-extension-settings"));
    m_scan_plugins_btn->setText(TR("settings-scan-plugins"));
    if (m_theme_picker != nullptr) {
        m_updating_theme_picker = true;
        int lightIdx = m_theme_picker->findData(QStringLiteral("light"));
        if (lightIdx >= 0) {
            m_theme_picker->setItemText(lightIdx, TR("settings-theme-light"));
        }
        int darkIdx = m_theme_picker->findData(QStringLiteral("dark"));
        if (darkIdx >= 0) {
            m_theme_picker->setItemText(darkIdx, TR("settings-theme-dark"));
        }
        m_updating_theme_picker = false;
    }
    rebuildPluginRows();
    rebuildSettingSectionRows();
}

} // namespace modal
