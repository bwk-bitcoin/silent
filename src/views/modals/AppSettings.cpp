#include "AppSettings.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Section.h"
#include "catalog/containers/SectionHeader.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/EmptyState.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Toggle.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "i18n/I18nManager.h"
#include "i18n/Tr.h"
#include "interfaces/instance.h"
#include "interfaces/module.h"
#include "interfaces/theme.h"
#include "theme/Icon.h"
#include "theme/Theme.h"
#include <QCheckBox>
#include <QEvent>
#include <QVariant>
#include <Qontrol>
#include <common.h>

namespace modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;
using catalog::Notice;

namespace {

const char *const K_PLUGIN_ID = "pluginId";
const char *const K_SECTION_ID = "sectionId";
const QString LIGHT = QStringLiteral("light");
const QString DARK = QStringLiteral("dark");

struct LanguageOption {
    const char *label;
    const char *code;
};

// Each language under its own name.
constexpr LanguageOption LANGUAGES[] = {
    {.label = "English", .code = "en"},   {.label = "Français", .code = "fr"},
    {.label = "Italiano", .code = "it"},  {.label = "Deutsch", .code = "de"},
    {.label = "Português", .code = "pt"}, {.label = "Español", .code = "es"},
};

auto foldable(const QString &label) -> catalog::Section * {
    auto *section = new catalog::Section(label);
    section->header()->setFoldable(true);
    return section;
}

// A field over its note, as wide as the wider of the two.
auto withNote(catalog::FieldGroup *field, catalog::FieldNote *note) -> QWidget * {
    field->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    return (new qontrol::Column)->spacing(resolve(Spacing::XS))->push(field)->push(note);
}

auto wrapped(LabelRole role) -> Label * {
    auto *label = new Label(role);
    label->setWordWrap(true);
    return label;
}

} // namespace

AppSettings::AppSettings() {
    init();
    doConnect();
    view();
    refreshThemeProviders();
    requestThemeList();
    applyPersistedTheme();
    showMain();
}

AppSettings::~AppSettings() {
    // Hand the module its widget back before the slot and its children go.
    if (m_open_section != nullptr) {
        m_section_slot->take(m_open_section.data());
    }
}

void AppSettings::init() {
    auto *controller = AppController::get();
    if (controller != nullptr) {
        m_plugin_registry = controller->pluginRegistry();
    }
    // The unavailable notice asks to reopen Settings once the folder is fixed: look again.
    if (m_plugin_registry != nullptr && !m_plugin_registry->isAvailable()) {
        m_plugin_registry->rescanDiscoveredPlugins();
    }
    m_host = Host::get();
    if (m_host != nullptr) {
        m_host_events = m_host->events();
    }
    initMain();
    initDisable();
    initSection();
}

void AppSettings::initMain() {
    m_theme_picker = new catalog::ComboBox;
    m_theme_picker->setWidth(Size::S);
    m_theme_picker->setEnabled(false);
    m_theme_note = new catalog::FieldNote;
    m_theme_note->setVisible(false);

    m_language_picker = new catalog::ComboBox;
    m_language_picker->setWidth(Size::S);
    for (const auto &language : LANGUAGES) {
        m_language_picker->addItem(QString::fromUtf8(language.label),
                                   QString::fromLatin1(language.code));
    }
    int localeIndex = m_language_picker->findData(i18n::I18nManager::get()->selectedLocale());
    if (localeIndex < 0) {
        localeIndex = m_language_picker->findData(QStringLiteral("en"));
    }
    m_language_picker->setCurrentIndex(localeIndex);
    m_language_note = new catalog::FieldNote(QString(), catalog::FieldNote::Tone::Success);
    m_language_note->setVisible(false);

    m_scan_btn = catalog::btnScanPlugins();
    m_scanning_btn = catalog::btnScanningPlugins();
    m_scanning_btn->setEnabled(false);
    m_scanning_btn->setVisible(false);
    m_registry_notice = new Notice(Notice::Tone::Error);
    m_plugin_notice = new Notice(Notice::Tone::Error);
    m_plugin_notice->setVisible(false);
    m_scan_result = new Label(LabelRole::Caption);
    m_scan_result->setTone(LabelTone::Success);
    m_scan_result->setVisible(false);
    m_plugins_column = (new qontrol::Column)->spacing(resolve(Spacing::S));
    m_plugins_empty = new catalog::EmptyState(icon::puzzle, QString());
    m_scanning = new catalog::ModalStatus;
    m_scanning->setSpinner();
    m_scanning->setPadding(resolve(Spacing::M), resolve(Spacing::M));
    m_scanning->setVisible(false);

    m_extensions_separator = new catalog::Separator;
    m_section_column = (new qontrol::Column)->spacing(resolve(Spacing::S));
}

void AppSettings::initDisable() {
    m_disable_question = wrapped(LabelRole::Body);
    m_disable_accounts = new Notice;
    m_disable_themes = new Notice;
    m_disable_actions = new catalog::ModalActions;
    m_disable_actions->setBack(catalog::ModalActions::Back::Back);
    m_disable_btn = catalog::btnDisablePlugin();
    m_disable_actions->addAction(m_disable_btn);
}

void AppSettings::initSection() {
    m_section_text = wrapped(LabelRole::Body);
    m_section_text->setTone(LabelTone::Secondary);
    m_section_slot = new qontrol::Column;
    m_section_actions = new catalog::ModalActions;
    m_section_actions->setBack(catalog::ModalActions::Back::AllSettings);
    m_done_btn = catalog::btnDone();
    m_section_actions->addAction(m_done_btn);
}

void AppSettings::doConnect() {
    connect(m_theme_picker, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &AppSettings::onThemeSelectionChanged, qontrol::UNIQUE);
    connect(m_language_picker, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &AppSettings::onLanguageSelectionChanged, qontrol::UNIQUE);
    connect(m_scan_btn, &QPushButton::clicked, this, &AppSettings::scanPlugins, qontrol::UNIQUE);
    connect(m_disable_btn, &QPushButton::clicked, this, &AppSettings::disablePlugin,
            qontrol::UNIQUE);
    connect(m_disable_actions, &catalog::ModalActions::backClicked, this,
            &AppSettings::onDisableBack, qontrol::UNIQUE);
    connect(m_done_btn, &QPushButton::clicked, this, &AppSettings::closeSection, qontrol::UNIQUE);
    connect(m_section_actions, &catalog::ModalActions::backClicked, this,
            &AppSettings::closeSection, qontrol::UNIQUE);
    if (m_plugin_registry != nullptr) {
        connect(m_plugin_registry, &PluginRegistry::enabledPluginsChanged, this,
                &AppSettings::rebuildPluginRows, qontrol::UNIQUE);
    }
    if (m_host_events != nullptr) {
        connect(m_host_events, &HostEvents::settingSectionsChanged, this,
                &AppSettings::onHostSettingsSectionsChanged, qontrol::UNIQUE);
        connect(m_host_events, &HostEvents::instanceRegistered, this,
                &AppSettings::onHostInstanceChanged, qontrol::UNIQUE);
        connect(m_host_events, &HostEvents::instanceRemoved, this,
                &AppSettings::onHostInstanceChanged, qontrol::UNIQUE);
    }
}

void AppSettings::view() {
    m_extensions = foldable(QString());
    m_extensions->setContent(m_section_column);
    m_main = new catalog::ScrollArea;
    m_main->setFitContent(true);
    m_main->setWidget((new qontrol::Column)
                          ->spacing(resolve(Spacing::S))
                          ->push(buildGeneral())
                          ->push(new catalog::Separator)
                          ->push(buildPlugins())
                          ->push(m_extensions_separator)
                          ->push(m_extensions));
    for (auto *section : {m_general, m_plugins, m_extensions}) {
        connect(section->header(), &catalog::SectionHeader::toggled, this, &AppSettings::adjustSize,
                qontrol::UNIQUE);
    }
    m_disable = buildDisable();
    m_section = new catalog::ScrollArea;
    m_section->setFitContent(true);
    m_section->setWidget(buildSection());
    // Owned by the modal, hidden until their view shows them.
    QList<QWidget *> views = {m_disable, m_disable_actions, m_section, m_section_actions};
    for (auto *widget : views) {
        widget->hide();
        widget->setParent(this);
    }
    retranslateUi();
}

auto AppSettings::buildGeneral() -> QWidget * {
    m_theme_field = new catalog::FieldGroup;
    m_theme_field->addField(QString(), m_theme_picker);
    m_language_field = new catalog::FieldGroup;
    m_language_field->addField(QString(), m_language_picker);

    m_general = foldable(QString());
    m_general->setContent((new qontrol::Row)
                              ->spacing(resolve(Spacing::XL))
                              ->push(withNote(m_theme_field, m_theme_note), 0, Qt::AlignTop)
                              ->push(withNote(m_language_field, m_language_note), 0, Qt::AlignTop)
                              ->pushSpacer());
    return m_general;
}

auto AppSettings::buildPlugins() -> QWidget * {
    m_plugins = foldable(QString());
    m_plugins->header()->setAction((new qontrol::Row)->push(m_scan_btn)->push(m_scanning_btn));
    m_plugins->setContent((new qontrol::Column)
                              ->spacing(resolve(Spacing::S))
                              ->push(m_registry_notice)
                              ->push(m_plugin_notice)
                              ->push(m_scan_result)
                              ->push(m_plugins_column)
                              ->push(m_plugins_empty)
                              ->push(m_scanning));
    return m_plugins;
}

auto AppSettings::buildDisable() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_disable_question)
        ->push(m_disable_accounts)
        ->push(m_disable_themes);
}

auto AppSettings::buildSection() -> QWidget * {
    auto *card = new catalog::Card(catalog::Card::Role::Inset);
    card->setPad(catalog::Card::Pad::M);
    card->setContent(m_section_slot);
    return (new qontrol::Column)->spacing(resolve(Spacing::S))->push(m_section_text)->push(card);
}

void AppSettings::onDisableBack() {
    // A Back from the disable view leaves the plugin enabled: its toggle shows it again.
    rebuildPluginRows();
    showMain();
}

void AppSettings::showMain() {
    setWindowTitle(TR("settings-title"));
    setHeader(nullptr);
    setBody(m_main);
    setFooter(nullptr);
    setWidth(ModalWidth::Wide);
}

void AppSettings::rebuildPluginRows() {
    m_plugins_column->clear();
    m_plugin_meta.clear();

    bool available = m_plugin_registry != nullptr && m_plugin_registry->isAvailable();
    m_registry_notice->setVisible(!available);
    m_scan_btn->setEnabled(available);
    int count = 0;
    if (available) {
        for (auto *plugin : m_plugin_registry->plugins()) {
            if (plugin == nullptr) {
                continue;
            }
            auto meta = plugin->meta();
            QString id = meta.value(QStringLiteral("id")).trimmed();
            // Built-in modules are not user-managed: only external plugins are listed.
            if (id.isEmpty() || m_plugin_registry->pluginHasBuiltinModules(id)) {
                continue;
            }
            QString version = meta.value(QStringLiteral("version")).trimmed();
            QString line = version.isEmpty() ? id : QStringLiteral("%1  v%2").arg(id, version);
            m_plugin_meta.insert(id, line);
            auto *row = pluginRow(id, line);
            m_plugins_column->push(row);
            // A row added to a shown column stays hidden until the event loop shows it,
            // so the step would be measured without it.
            row->show();
            ++count;
        }
    }
    m_plugins->header()->setCount(count > 0 ? QString::number(count) : QString());
    m_plugins_column->setVisible(count > 0);
    m_plugins_empty->setVisible(available && count == 0 && !m_scanning_plugins);
    m_scanning->setVisible(m_scanning_plugins && count == 0);
    adjustSize();
}

auto AppSettings::pluginRow(const QString &id, const QString &meta) -> catalog::SelectRow * {
    QString error = m_plugin_registry->pluginLoadError(id);
    bool failed = !error.isEmpty();
    bool unloading = id == m_unloading_id;

    auto *row = new catalog::SelectRow;
    row->setIcon(failed ? icon::circleX : icon::puzzle);
    row->setTone(failed ? catalog::SelectRow::Tone::Error : catalog::SelectRow::Tone::Default);
    row->setTitle(m_plugin_registry->pluginName(id));
    row->setSub(failed ? error : m_plugin_registry->pluginDescription(id));
    row->setMeta(meta);
    if (unloading) {
        row->setStatus(catalog::pillPluginUnloading());
    }
    // Kept when empty, so the meta lines up on every row.
    row->setTrailingWidth(metric::TOGGLE_WIDTH);
    row->setChevron(false);
    row->setClickable(false);
    // A plugin that failed to load has nothing to switch.
    if (!failed) {
        auto *toggle = new catalog::Toggle;
        toggle->setChecked(m_plugin_registry->isPluginEnabled(id));
        toggle->setEnabled(!unloading);
        toggle->setProperty(K_PLUGIN_ID, id);
        connect(toggle, &QCheckBox::toggled, this, &AppSettings::onPluginToggled, qontrol::UNIQUE);
        row->setTrailing(toggle);
    }
    return row;
}

void AppSettings::onPluginToggled(bool enabled) {
    auto *toggle = qobject_cast<catalog::Toggle *>(sender());
    if (toggle == nullptr || m_plugin_registry == nullptr) {
        return;
    }
    QString id = toggle->property(K_PLUGIN_ID).toString();
    m_plugin_notice->setVisible(false);
    m_scan_result->setVisible(false);
    if (!enabled) {
        showDisable(id);
        return;
    }
    // A plugin that fails to load gets its error row; a loaded one rebuilds the rows
    // through enabledPluginsChanged.
    if (!m_plugin_registry->setEnabled(id, true)) {
        rebuildPluginRows();
    }
}

void AppSettings::showDisable(const QString &id) {
    m_disabling_id = id;
    QString name = m_plugin_registry->pluginName(id);
    m_disable_question->setText(TR("settings-disable-question").arg(name, m_plugin_meta.value(id)));

    QStringList accounts = m_plugin_registry->pluginAccountNames(id);
    if (accounts.isEmpty()) {
        m_disable_accounts->setTone(Notice::Tone::Info);
        m_disable_accounts->setTitle(QString());
        m_disable_accounts->setText(TR("settings-disable-no-account"));
    } else if (accounts.size() == 1) {
        m_disable_accounts->setTone(Notice::Tone::Warning);
        m_disable_accounts->setTitle(TR("settings-disable-account-one"));
        m_disable_accounts->setText(TR("settings-disable-account-one-text").arg(accounts.first()));
    } else {
        m_disable_accounts->setTone(Notice::Tone::Warning);
        m_disable_accounts->setTitle(TR("settings-disable-accounts").arg(accounts.size()));
        m_disable_accounts->setText(
            TR("settings-disable-accounts-text").arg(accounts.join(QStringLiteral(", "))));
    }

    QStringList themes;
    for (auto it = m_theme_plugin.constBegin(); it != m_theme_plugin.constEnd(); ++it) {
        if (it.value() == id) {
            themes.append(it.key());
        }
    }
    themes.sort();
    m_disable_themes->setTone(Notice::Tone::Info);
    m_disable_themes->setText(TR("settings-disable-themes").arg(themes.join(QStringLiteral(", "))));
    m_disable_themes->setVisible(!themes.isEmpty());

    setWindowTitle(TR("settings-disable-title"));
    setHeader(nullptr);
    setBody(m_disable);
    setFooter(m_disable_actions);
    setWidth(ModalWidth::Wide);
}

void AppSettings::disablePlugin() {
    QString id = m_disabling_id;
    QString activeTheme = m_theme_picker->currentData().toString();
    bool themeGoes = !id.isEmpty() && m_theme_plugin.value(activeTheme) == id;

    m_unloading_id = id;
    rebuildPluginRows();
    showMain();
    // Unloading runs on this thread: paint the Unloading row before it starts.
    repaint();
    bool disabled = m_plugin_registry->setEnabled(id, false);
    m_unloading_id.clear();

    if (!disabled) {
        m_plugin_notice->setTitle(
            TR("settings-disable-refused").arg(m_plugin_registry->pluginName(id)));
        m_plugin_notice->setText(TR("settings-disable-refused-text"));
        m_plugin_notice->setVisible(true);
    } else if (themeGoes) {
        switchToLightTheme();
    }
    rebuildPluginRows();
}

void AppSettings::setScanning(bool scanning) {
    m_scanning_plugins = scanning;
    m_scan_btn->setVisible(!scanning);
    m_scanning_btn->setVisible(scanning);
    rebuildPluginRows();
}

void AppSettings::scanPlugins() {
    if (m_plugin_registry == nullptr) {
        return;
    }
    m_plugin_notice->setVisible(false);
    m_scan_result->setVisible(false);
    setScanning(true);
    // The scan reads the plugins folder on this thread: paint Scanning before it starts.
    repaint();
    int found = m_plugin_registry->rescanDiscoveredPlugins();
    if (found > 0) {
        m_scan_result->setText(found == 1 ? TR("settings-plugin-found-one")
                                          : TR("settings-plugins-found").arg(found));
        m_scan_result->setVisible(true);
    }
    setScanning(false);
}

void AppSettings::rebuildSettingSectionRows() {
    m_section_column->clear();
    QList<Host::SettingSectionView> sections;
    if (m_host != nullptr) {
        sections = m_host->settingSections();
    }

    int count = 0;
    for (const auto &entry : sections) {
        if (entry.content == nullptr) {
            continue;
        }
        QString title = entry.title.trimmed().isEmpty()
                            ? TR("settings-section-fallback").arg(entry.id)
                            : entry.title.trimmed();
        QString module = sectionModuleName(entry.id);

        auto *open = catalog::btnOpen();
        open->setProperty(K_SECTION_ID, QVariant::fromValue(entry.id));
        connect(open, &QPushButton::clicked, this, &AppSettings::onOpenSectionClicked,
                qontrol::UNIQUE);
        auto *row = new catalog::SelectRow;
        row->setIcon(icon::settings);
        row->setTitle(title);
        row->setSub(module.isEmpty() ? QString() : TR("settings-section-from").arg(module));
        row->setTrailing(open);
        row->setProperty(K_SECTION_ID, QVariant::fromValue(entry.id));
        connect(row, &catalog::SelectRow::clicked, this, &AppSettings::onOpenSectionClicked,
                qontrol::UNIQUE);
        m_section_column->push(row);
        row->show();
        ++count;
    }
    m_extensions->header()->setCount(QString::number(count));
    m_extensions->setVisible(count > 0);
    m_extensions_separator->setVisible(count > 0);
    adjustSize();
}

auto AppSettings::sectionModuleName(Host::SectionId section) const -> QString {
    auto *hostImpl = dynamic_cast<HostImpl *>(m_host);
    if (hostImpl == nullptr) {
        return {};
    }
    auto *module = hostImpl->settingSectionModule(section);
    if (module == nullptr) {
        return {};
    }
    return module->meta().value(QStringLiteral("name")).trimmed();
}

void AppSettings::onOpenSectionClicked() {
    if (sender() == nullptr || m_host == nullptr) {
        return;
    }
    auto sectionId = sender()->property(K_SECTION_ID).value<Host::SectionId>();
    // Re-queried rather than kept: a module can drop its section while Settings is open.
    for (const auto &entry : m_host->settingSections()) {
        if (entry.id != sectionId || entry.content == nullptr) {
            continue;
        }
        QString title = entry.title.trimmed().isEmpty()
                            ? TR("settings-section-fallback").arg(entry.id)
                            : entry.title.trimmed();
        QString module = sectionModuleName(entry.id);
        m_section_text->setText(
            TR("settings-section-belongs").arg(module.isEmpty() ? title : module));
        m_open_section = entry.content;
        m_open_section_id = entry.id;
        m_section_slot->push(entry.content);
        entry.content->show();
        showSection(title);
        return;
    }
}

void AppSettings::showSection(const QString &title) {
    setWindowTitle(title);
    setHeader(nullptr);
    setBody(m_section);
    setFooter(m_section_actions);
    setWidth(ModalWidth::Wide);
}

void AppSettings::closeSection() {
    if (m_open_section != nullptr) {
        m_section_slot->take(m_open_section.data());
    }
    m_open_section = nullptr;
    m_open_section_id = 0;
    showMain();
}

void AppSettings::onHostSettingsSectionsChanged() {
    rebuildSettingSectionRows();
    if (m_open_section_id == 0 || m_host == nullptr) {
        return;
    }
    for (const auto &entry : m_host->settingSections()) {
        if (entry.id == m_open_section_id) {
            return;
        }
    }
    // The module dropped the section on display.
    closeSection();
}

void AppSettings::refreshThemeProviders() {
    m_theme_providers.clear();
    m_provider_plugin.clear();
    m_pending_theme_list.clear();
    m_pending_palette.clear();
    m_provider_for_theme_name.clear();
    m_theme_plugin.clear();

    if (m_host == nullptr) {
        return;
    }

    for (auto *instance : m_host->instances()) {
        if (instance == nullptr) {
            continue;
        }
        auto *provider = instance->theme();
        if (provider == nullptr || !provider->implemented() ||
            m_theme_providers.contains(provider)) {
            continue;
        }
        m_theme_providers.append(provider);
        if (m_plugin_registry != nullptr) {
            m_provider_plugin.insert(provider,
                                     m_plugin_registry->instanceOwnerPluginId(instance->id()));
        }
        connect(provider, qOverload<ReqId, QStringList>(&IThemeProvider::themes), this,
                &AppSettings::onThemeNames, qontrol::UNIQUE);
        connect(provider, qOverload<ReqId, theme::Palette>(&IThemeProvider::palette), this,
                &AppSettings::onThemePalette, qontrol::UNIQUE);
    }
}

void AppSettings::requestThemeList() {
    m_updating_theme_picker = true;
    m_theme_picker->clear();
    m_theme_picker->addItem(TR("settings-theme-light"), LIGHT);
    m_theme_picker->addItem(TR("settings-theme-dark"), DARK);
    m_updating_theme_picker = false;

    for (auto *provider : m_theme_providers) {
        ReqId reqId = provider->themes();
        if (reqId != 0) {
            m_pending_theme_list.insert(reqId, provider);
        }
    }

    m_theme_picker->setEnabled(true);
    updateThemeNote();
}

void AppSettings::applyPersistedTheme() {
    QString activeTheme = QString::fromStdString(std::string(app_active_theme().c_str())).trimmed();
    if (activeTheme.isEmpty()) {
        return;
    }

    if (activeTheme == LIGHT || activeTheme == DARK) {
        int index = m_theme_picker->findData(activeTheme);
        if (index >= 0) {
            setThemePickerIndexSilently(index);
        }
        if (auto *theme = Theme::get(); theme != nullptr) {
            theme->setMode(activeTheme == DARK ? ThemeMode::Dark : ThemeMode::Light);
        }
        return;
    }

    m_pending_theme_to_apply = activeTheme;
    int index = m_theme_picker->findData(activeTheme);
    if (index >= 0) {
        setThemePickerIndexSilently(index);
    }

    auto *provider = m_provider_for_theme_name.value(activeTheme, nullptr);
    if (provider != nullptr) {
        ReqId reqId = provider->palette(activeTheme);
        if (reqId != 0) {
            m_pending_palette.insert(reqId, provider);
        }
    }
}

void AppSettings::setThemePickerIndexSilently(int index) {
    m_updating_theme_picker = true;
    m_theme_picker->setCurrentIndex(index);
    m_updating_theme_picker = false;
    updateThemeNote();
}

void AppSettings::updateThemeNote() {
    QString pluginId = m_theme_plugin.value(m_theme_picker->currentData().toString());
    if (pluginId.isEmpty() || m_plugin_registry == nullptr) {
        m_theme_note->setVisible(false);
        return;
    }
    m_theme_note->setText(
        TR("settings-theme-provided-by").arg(m_plugin_registry->pluginName(pluginId)));
    m_theme_note->setVisible(true);
}

void AppSettings::switchToLightTheme() {
    m_pending_theme_to_apply.clear();
    if (auto *theme = Theme::get(); theme != nullptr) {
        theme->setMode(ThemeMode::Light);
    }
    app_set_active_theme(rust::String(LIGHT.toStdString()));
    int index = m_theme_picker->findData(LIGHT);
    if (index >= 0) {
        setThemePickerIndexSilently(index);
    }
}

void AppSettings::onThemeSelectionChanged(int index) {
    if (m_updating_theme_picker || index < 0) {
        return;
    }
    updateThemeNote();
    adjustSize();

    QString themeName = m_theme_picker->itemData(index).toString().trimmed();
    if (themeName.isEmpty()) {
        return;
    }

    if (themeName == LIGHT || themeName == DARK) {
        if (auto *theme = Theme::get(); theme != nullptr) {
            theme->setMode(themeName == DARK ? ThemeMode::Dark : ThemeMode::Light);
        }
        app_set_active_theme(rust::String(themeName.toStdString()));
        return;
    }

    auto *provider = m_provider_for_theme_name.value(themeName, nullptr);
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
    if (index < 0) {
        return;
    }
    QString locale = m_language_picker->itemData(index).toString().trimmed();
    if (locale.isEmpty()) {
        return;
    }
    static_cast<void>(i18n::I18nManager::get()->applyLocale(locale, true));
    m_language_note->setText(TR("settings-language-saved"));
    m_language_note->setVisible(true);
    adjustSize();
}

void AppSettings::onThemeNames(ReqId req_id, const QStringList &themes) {
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
            m_theme_plugin.insert(themeName, m_provider_plugin.value(provider));
        }
        if (m_theme_picker->findData(themeName) < 0) {
            m_updating_theme_picker = true;
            m_theme_picker->addItem(themeName, themeName);
            m_updating_theme_picker = false;
        }
    }

    if (m_pending_theme_to_apply.isEmpty()) {
        return;
    }
    int activeIndex = m_theme_picker->findData(m_pending_theme_to_apply);
    if (activeIndex < 0) {
        return;
    }
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

void AppSettings::onHostInstanceChanged([[maybe_unused]] const QString &id) {
    refreshThemeProviders();
    requestThemeList();
    applyPersistedTheme();
}

void AppSettings::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void AppSettings::retranslateUi() {
    // A module's section keeps the title it gave.
    if (m_main->isVisibleTo(this)) {
        setWindowTitle(TR("settings-title"));
    } else if (m_disable->isVisibleTo(this)) {
        setWindowTitle(TR("settings-disable-title"));
    }

    m_general->header()->setLabel(TR("settings-general"));
    m_theme_field->setFieldLabel(m_theme_picker, TR("settings-theme"));
    m_language_field->setFieldLabel(m_language_picker, TR("settings-language"));
    m_updating_theme_picker = true;
    m_theme_picker->setItemText(m_theme_picker->findData(LIGHT), TR("settings-theme-light"));
    m_theme_picker->setItemText(m_theme_picker->findData(DARK), TR("settings-theme-dark"));
    m_updating_theme_picker = false;
    updateThemeNote();
    m_language_note->setText(TR("settings-language-saved"));

    // Helper buttons keep the language they were built in: retitle them here.
    m_plugins->header()->setLabel(TR("settings-plugin-manager"));
    m_scan_btn->setText(TR("btn-scan-for-plugins"));
    m_scanning_btn->setText(TR("btn-scanning"));
    m_registry_notice->setTitle(TR("settings-registry-unavailable"));
    m_registry_notice->setText(TR("settings-registry-unavailable-text"));
    m_plugins_empty->setTitle(TR("settings-no-plugin"));
    m_plugins_empty->setSubtitle(TR("settings-no-plugin-text"));
    m_scanning->setTitle(TR("settings-scanning-plugins"));
    m_extensions->header()->setLabel(TR("settings-extension-settings"));

    m_disable_actions->setBack(catalog::ModalActions::Back::Back);
    m_disable_btn->setText(TR("btn-disable-plugin"));
    m_section_actions->setBack(catalog::ModalActions::Back::AllSettings);
    m_done_btn->setText(TR("btn-done"));

    rebuildPluginRows();
    rebuildSettingSectionRows();
}

} // namespace modal
