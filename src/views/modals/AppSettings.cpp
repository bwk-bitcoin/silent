#include "AppSettings.h"

#include "appsettings/AccessFlow.h"
#include "appsettings/DisableFlow.h"
#include "appsettings/SectionFlow.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/ClearRows.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/Section.h"
#include "catalog/containers/SectionHeader.h"
#include "catalog/containers/Separator.h"
#include "catalog/containers/ViewSlots.h"
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
#include "host/PluginAccess.h"
#include "host/PluginRegistry.h"
#include "i18n/I18nManager.h"
#include "i18n/Tr.h"
#include "interfaces/instance.h"
#include "interfaces/module.h"
#include "interfaces/theme.h"
#include "theme/Icon.h"
#include "theme/Theme.h"
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QHideEvent>
#include <QLabel>
#include <QMimeData>
#include <QUrl>
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

} // namespace

AppSettings::AppSettings(QWidget *parent) : QWidget(parent) {
    init();
    doConnect();
    view();
    refreshThemeProviders();
    requestThemeList();
    applyPersistedTheme();
    showMain();
}

AppSettings::~AppSettings() {
    closeFlow();
}

void AppSettings::init() {
    auto *controller = AppController::get();
    if (controller != nullptr) {
        m_plugin_registry = controller->pluginRegistry();
    }
    m_host = Host::get();
    if (m_host != nullptr) {
        m_host_events = m_host->events();
    }
    initMain();
}

void AppSettings::doConnect() {
    if (m_host_events != nullptr) {
        connect(m_host_events, &HostEvents::instanceRegistered, this,
                &AppSettings::onHostInstanceChanged, qontrol::UNIQUE);
        connect(m_host_events, &HostEvents::instanceRemoved, this,
                &AppSettings::onHostInstanceChanged, qontrol::UNIQUE);
    }
    connectMain();
}

void AppSettings::view() {
    m_main = viewMain();
    // As the Keys page: a centred column under its title, its width the view's.
    // Its views keep their height, top-packed; the main one scrolls when the window is
    // shorter.
    m_title = new Label(TR("settings-title"), LabelRole::Title);
    m_title_row = (new qontrol::Row)->push(m_title)->pushSpacer();
    m_slots = new catalog::ViewSlots;
    m_column = (new qontrol::Column)
                   ->margins(resolve(Spacing::S))
                   ->spacing(resolve(Spacing::L))
                   ->push(m_title_row)
                   ->push(m_slots)
                   ->pushStretch(1);
    (new qontrol::Row)->pushSpacer()->push(m_column)->pushSpacer()->into(this);
    retranslateUi();
}

// ===== Main view =====

void AppSettings::initMain() {
    m_theme_picker = new catalog::ComboBox;
    m_theme_picker->setFixedWidth(metric::SETTINGS_PICKER_WIDTH);
    m_theme_picker->setEnabled(false);
    m_theme_note = new catalog::FieldNote;
    m_theme_note->setVisible(false);

    m_language_picker = new catalog::ComboBox;
    m_language_picker->setFixedWidth(metric::SETTINGS_PICKER_WIDTH);
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

    m_plugin_drop = new catalog::Card(catalog::Card::Role::Inset);
    m_plugin_drop->setPad(catalog::Card::Pad::M);
    auto *dropIcon = new QLabel;
    dropIcon->setPixmap(
        icon::puzzle(IconColor::Muted).pixmap(metric::DROP_ICON_SIZE, metric::DROP_ICON_SIZE));
    m_plugin_drop_caption = new Label(QString(), LabelRole::Caption);
    m_plugin_drop_caption->setTone(LabelTone::Muted);
    m_plugin_drop_caption->setWordWrap(true);
    m_plugin_drop_caption->setAlignment(Qt::AlignHCenter);
    m_plugin_drop->setContent((new qontrol::Column)
                                  ->margins(0, resolve(Spacing::M), 0, resolve(Spacing::M))
                                  ->spacing(resolve(Spacing::S))
                                  ->pushAligned(dropIcon, Qt::AlignHCenter)
                                  // Full width: a wrapped caption pushed centered is
                                  // measured too narrow and loses its last line.
                                  ->push(m_plugin_drop_caption));
    m_plugin_notice = new Notice(Notice::Tone::Error);
    m_plugin_notice->setVisible(false);
    m_plugin_result = new Label(LabelRole::Caption);
    m_plugin_result->setTone(LabelTone::Success);
    m_plugin_result->setWordWrap(true);
    m_plugin_result->setVisible(false);
    m_plugins_column = (new qontrol::Column)->spacing(resolve(Spacing::S));
    setAcceptDrops(true);

    m_extensions_separator = new catalog::Separator;
    m_section_column = (new qontrol::Column)->spacing(resolve(Spacing::S));
}

void AppSettings::connectMain() {
    connect(m_theme_picker, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &AppSettings::onThemeSelectionChanged, qontrol::UNIQUE);
    connect(m_language_picker, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &AppSettings::onLanguageSelectionChanged, qontrol::UNIQUE);
    if (m_plugin_registry != nullptr) {
        connect(m_plugin_registry, &PluginRegistry::enabledPluginsChanged, this,
                &AppSettings::onPluginsChanged, qontrol::UNIQUE);
    }
    if (m_host_events != nullptr) {
        connect(m_host_events, &HostEvents::settingSectionsChanged, this,
                &AppSettings::onHostSettingsSectionsChanged, qontrol::UNIQUE);
    }
}

auto AppSettings::viewMain() -> catalog::ScrollArea * {
    m_extensions = foldable(QString());
    m_extensions->setContent(m_section_column);
    auto *column = (new qontrol::Column)
                       ->spacing(resolve(Spacing::S))
                       ->push(buildGeneral())
                       ->push(new catalog::Separator)
                       ->push(buildPlugins())
                       ->push(m_extensions_separator)
                       ->push(m_extensions);
    auto *scroll = new catalog::ScrollArea;
    scroll->setFitContent(true);
    // One width whatever the rows hold, as the design's column: longer texts wrap in it.
    column->setFixedWidth(metric::SETTINGS_BODY_WIDTH);
    scroll->setWidget(column);
    return scroll;
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
    m_plugins->setContent((new qontrol::Column)
                              ->spacing(resolve(Spacing::S))
                              ->push(m_plugin_notice)
                              ->push(m_plugin_result)
                              ->push(m_plugins_column)
                              ->push(m_plugin_drop));
    return m_plugins;
}

void AppSettings::showMain() {
    m_slots->setHeader(nullptr);
    m_slots->setBody(m_main);
    m_slots->setFooter(nullptr);
    // The sections sit in the scroll area, inset by its gutters: the title too.
    setView(TR("settings-title"), metric::SETTINGS_BODY_WIDTH + (2 * catalog::ScrollArea::insetH()),
            catalog::ScrollArea::insetH());
}

void AppSettings::setView(const QString &title, int width, int title_inset) {
    m_title->setText(title);
    m_title_row->layout()->setContentsMargins(title_inset, 0, 0, 0);
    m_column->setFixedWidth(width + (2 * resolve(Spacing::S)));
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
    } else if (!pluginaccess::forPlugin(id).isEmpty()) {
        // What it may use, once it has asked for something.
        auto *accessBtn = catalog::btnPluginAccess();
        accessBtn->setProperty(K_PLUGIN_ID, id);
        connect(accessBtn, &QPushButton::clicked, this, &AppSettings::onPluginAccessClicked,
                qontrol::UNIQUE);
        row->setStatus(accessBtn);
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

void AppSettings::rebuildSettingSectionRows() {
    // clear() also resets the layout spacing.
    catalog::clearRows(m_section_column);
    m_section_column->spacing(resolve(Spacing::S));
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

    auto *hostImpl = dynamic_cast<HostImpl *>(m_host);
    if (hostImpl == nullptr) {
        return;
    }
    for (auto *instance : hostImpl->liveInstances()) {
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

// ===== Shared =====

void AppSettings::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

void AppSettings::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    // Another tab shown, not the window minimized: a flow left open goes back to the
    // main view, and a result shown is done with.
    if (event->spontaneous()) {
        return;
    }
    if (m_flow != nullptr) {
        onFlowBack();
    }
    m_plugin_notice->setVisible(false);
    m_plugin_result->setVisible(false);
}

void AppSettings::retranslateUi() {
    // The language is picked on the main view: a flow is always opened in it.
    if (m_flow == nullptr) {
        m_title->setText(TR("settings-title"));
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
    m_plugin_drop_caption->setText(TR("settings-plugin-drop"));
    m_extensions->header()->setLabel(TR("settings-extension-settings"));

    onPluginsChanged();
    rebuildSettingSectionRows();
}

void AppSettings::openFlow(AppSettingsFlow *flow) {
    m_flow = flow;
    connect(m_flow, &AppSettingsFlow::viewChanged, this, &AppSettings::onFlowViewChanged,
            qontrol::UNIQUE);
    connect(m_flow, &AppSettingsFlow::back, this, &AppSettings::onFlowBack, qontrol::UNIQUE);
    connect(m_flow, &AppSettingsFlow::confirmed, this, &AppSettings::onFlowConfirmed,
            qontrol::UNIQUE);
    m_slots->setPage(m_flow);
    m_flow->start();
}

void AppSettings::closeFlow() {
    if (m_flow == nullptr) {
        return;
    }
    // Deleted later: the flow may still be in the slot that emitted.
    m_flow->deleteLater();
    m_flow = nullptr;
}

// ===== Slots =====

void AppSettings::onThemeSelectionChanged(int index) {
    if (m_updating_theme_picker || index < 0) {
        return;
    }
    updateThemeNote();

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
}

void AppSettings::dragEnterEvent(QDragEnterEvent *event) {
    if (m_flow == nullptr && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

// Each dropped folder or library is sealed into the state: the plugin is listed,
// off, until turned on.
void AppSettings::dropEvent(QDropEvent *event) {
    if (m_flow != nullptr || m_plugin_registry == nullptr || !event->mimeData()->hasUrls()) {
        return;
    }
    event->acceptProposedAction();
    m_plugin_notice->setVisible(false);
    m_plugin_result->setVisible(false);
    QStringList added;
    QStringList updated;
    for (const auto &url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        auto result = ::plugin_install(rust::String(url.toLocalFile().toStdString()));
        if (!result.is_ok) {
            m_plugin_notice->setTitle(TR("settings-plugin-add-failed"));
            m_plugin_notice->setText(QString::fromStdString(std::string(result.error)));
            m_plugin_notice->setVisible(true);
            continue;
        }
        QString id = QString::fromStdString(std::string(result.plugin_id));
        (m_plugin_registry->isPluginEnabled(id) ? updated : added).append(id);
    }
    m_plugin_registry->rescanDiscoveredPlugins();
    QStringList lines;
    for (const auto &id : added) {
        lines.append(TR("settings-plugin-added").arg(m_plugin_registry->pluginName(id)));
    }
    for (const auto &id : updated) {
        lines.append(TR("settings-plugin-updated").arg(m_plugin_registry->pluginName(id)));
    }
    m_plugin_result->setText(lines.join(QLatin1Char('\n')));
    m_plugin_result->setVisible(!lines.isEmpty());
}

void AppSettings::onPluginToggled(bool enabled) {
    auto *toggle = qobject_cast<catalog::Toggle *>(sender());
    if (toggle == nullptr || m_plugin_registry == nullptr) {
        return;
    }
    QString id = toggle->property(K_PLUGIN_ID).toString();
    m_plugin_notice->setVisible(false);
    m_plugin_result->setVisible(false);
    if (!enabled) {
        m_disabling_id = id;
        openFlow(new DisableFlow(m_plugin_registry, id, m_plugin_meta.value(id), m_theme_plugin));
        return;
    }
    // A plugin that fails to load gets its error row; a loaded one rebuilds the rows
    // through enabledPluginsChanged.
    if (!m_plugin_registry->setEnabled(id, true)) {
        onPluginsChanged();
    }
}

void AppSettings::onPluginAccessClicked() {
    if (sender() == nullptr || m_plugin_registry == nullptr) {
        return;
    }
    m_plugin_notice->setVisible(false);
    m_plugin_result->setVisible(false);
    openFlow(new AccessFlow(m_plugin_registry, sender()->property(K_PLUGIN_ID).toString()));
}

void AppSettings::onPluginsChanged() {
    // clear() also resets the layout spacing.
    catalog::clearRows(m_plugins_column);
    m_plugins_column->spacing(resolve(Spacing::S));
    m_plugin_meta.clear();

    bool available = m_plugin_registry != nullptr;
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
        openFlow(new SectionFlow(
            m_host, entry.id, title,
            TR("settings-section-belongs").arg(module.isEmpty() ? title : module), entry.content));
        return;
    }
}

void AppSettings::onHostSettingsSectionsChanged() {
    rebuildSettingSectionRows();
}

void AppSettings::onHostInstanceChanged([[maybe_unused]] const QString &id) {
    refreshThemeProviders();
    requestThemeList();
    applyPersistedTheme();
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

void AppSettings::onFlowViewChanged(const QString &title, ModalWidth width) {
    // The width its content had in the window, without the window's padding.
    setView(title, resolve(width) - (2 * resolve(Padding::L)), 0);
}

void AppSettings::onFlowBack() {
    // A Back from disabling leaves the plugin enabled: its toggle shows it again.
    onPluginsChanged();
    showMain();
    closeFlow();
}

// Only disabling a plugin asks to be confirmed.
void AppSettings::onFlowConfirmed() {
    QString id = m_disabling_id;
    QString activeTheme = m_theme_picker->currentData().toString();
    bool themeGoes = !id.isEmpty() && m_theme_plugin.value(activeTheme) == id;

    m_unloading_id = id;
    onPluginsChanged();
    showMain();
    closeFlow();
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
    onPluginsChanged();
}

// ===== AppSettingsFlow =====

AppSettingsFlow::AppSettingsFlow(QWidget *parent) : QWidget(parent) {
    m_slots = new catalog::ViewSlots;
    (new qontrol::Column)->push(m_slots)->into(this);
}

auto AppSettingsFlow::viewSlots() const -> catalog::ViewSlots * {
    return m_slots;
}

} // namespace modal
