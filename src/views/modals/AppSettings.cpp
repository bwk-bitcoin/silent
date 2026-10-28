#include "AppSettings.h"

#include "appsettings/AccessFlow.h"
#include "appsettings/DisableFlow.h"
#include "appsettings/RemoveFlow.h"
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
#include "host/PluginMetadata.h"
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
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <optional>

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

auto feeRecommendation(const std::optional<double> &rate) -> QString {
    return rate.has_value() ? QStringLiteral("%1 sat/vB").arg(QString::number(*rate, 'g', 15))
                            : QStringLiteral("-");
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
    // Its result is never delivered to a deleted page, but the import is not cut short.
    if (m_import_worker != nullptr) {
        m_import_worker->wait();
    }
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

    initPrices();
    initFees();

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
    m_removal_notice = new Notice(Notice::Tone::Error);
    m_removal_notice->setObjectName(QStringLiteral("pendingRemovalReadError"));
    m_removal_notice->setVisible(false);
    m_removal_retry_btn = catalog::btnTryAgain();
    m_removal_retry_btn->setObjectName(QStringLiteral("retryPendingRemovals"));
    m_removal_retry_btn->setVisible(false);
    m_removal_notice->setAction(m_removal_retry_btn);
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
    connectPrices();
    connectFees();
    connect(m_removal_retry_btn, &QPushButton::clicked, this, &AppSettings::onRetryPluginState,
            qontrol::UNIQUE);
    connect(m_theme_picker, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &AppSettings::onThemeSelectionChanged, qontrol::UNIQUE);
    connect(m_language_picker, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &AppSettings::onLanguageSelectionChanged, qontrol::UNIQUE);
    if (m_plugin_registry != nullptr) {
        connect(m_plugin_registry, &PluginRegistry::enabledPluginsChanged, this,
                &AppSettings::onPluginsChanged, qontrol::UNIQUE);
        connect(m_plugin_registry, &PluginRegistry::pluginStateChanged, this,
                &AppSettings::onPluginsChanged, qontrol::UNIQUE);
        connect(m_plugin_registry, &PluginRegistry::pluginStopFinished, this,
                &AppSettings::onPluginStopFinished, qontrol::UNIQUE);
    }
    connect(this, &AppSettings::bundlesImported, this, &AppSettings::onBundlesImported,
            qontrol::UNIQUE);
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
                       ->push(m_prices_separator)
                       ->push(m_prices)
                       ->push(new catalog::Separator)
                       ->push(m_fees)
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

void AppSettings::initPrices() {
    m_price_source_picker = new catalog::ComboBox;
    m_price_currency_picker = new catalog::ComboBox;
    m_price_source_picker->setMinimumWidth(metric::SETTINGS_PICKER_WIDTH);
    m_price_source_picker->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_price_source_picker->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_price_currency_picker->setFixedWidth(metric::SETTINGS_PICKER_WIDTH);
    m_price_fields = new catalog::FieldGroup;
    m_price_fields->addField(TR("settings-price-source"), m_price_source_picker);
    m_price_fields->addField(TR("settings-price-currency"), m_price_currency_picker);
    m_prices = foldable(TR("settings-prices"));
    m_prices->setContent(m_price_fields);
    m_prices_separator = new catalog::Separator;
    onPriceSourcesChanged();
}

void AppSettings::connectPrices() {
    connect(m_price_source_picker, qOverload<int>(&QComboBox::activated), this,
            &AppSettings::onPriceSourceActivated, qontrol::UNIQUE);
    connect(m_price_currency_picker, qOverload<int>(&QComboBox::activated), this,
            &AppSettings::onPriceCurrencyActivated, qontrol::UNIQUE);
    connect(m_host_events, &HostEvents::priceSourcesChanged, this,
            &AppSettings::onPriceSourcesChanged, qontrol::UNIQUE);
    connect(m_host_events, &HostEvents::selectedPriceChanged, this,
            &AppSettings::onPriceSourcesChanged, qontrol::UNIQUE);
}

void AppSettings::initFees() {
    m_fee_mode_picker = new catalog::ComboBox;
    m_fee_mode_picker->setObjectName(QStringLiteral("settings-fee-mode"));
    m_fee_source_picker = new catalog::ComboBox;
    m_fee_source_picker->setObjectName(QStringLiteral("settings-fee-source"));
    m_fee_mode_field = new catalog::FieldGroup;
    m_fee_mode_field->addField(TR("settings-fee-mode"), m_fee_mode_picker);
    m_fee_source_field = new catalog::FieldGroup;
    m_fee_source_field->addField(TR("settings-fee-source"), m_fee_source_picker);
    m_fee_default_label = new Label;
    m_fee_default_label->setObjectName(QStringLiteral("settings-fee-default"));
    m_fee_low_label = new Label;
    m_fee_low_label->setObjectName(QStringLiteral("settings-fee-low"));
    m_fee_medium_label = new Label;
    m_fee_medium_label->setObjectName(QStringLiteral("settings-fee-medium"));
    m_fee_fast_label = new Label;
    m_fee_fast_label->setObjectName(QStringLiteral("settings-fee-fast"));
    m_fee_recommendations = new catalog::FieldGroup;
    m_fee_recommendations->addField(TR("fee-default"), m_fee_default_label)
        ->addField(TR("fee-low"), m_fee_low_label)
        ->addField(TR("fee-medium"), m_fee_medium_label)
        ->addField(TR("fee-fast"), m_fee_fast_label);
    m_fees = foldable(TR("settings-fees"));
    m_fees->setContent((new qontrol::Column)
                           ->spacing(resolve(Spacing::S))
                           ->push(m_fee_mode_field)
                           ->push(m_fee_source_field)
                           ->push(m_fee_recommendations));
    onFeeSourcesChanged();
}

void AppSettings::connectFees() {
    for (auto *picker : {m_fee_mode_picker, m_fee_source_picker}) {
        connect(picker, qOverload<int>(&QComboBox::activated), this,
                &AppSettings::onFeePreferencesActivated, qontrol::UNIQUE);
    }
    connect(m_host_events, &HostEvents::feeSourcesChanged, this, &AppSettings::onFeeSourcesChanged,
            qontrol::UNIQUE);
    connect(m_host_events, &HostEvents::selectedFeesChanged, this,
            &AppSettings::onFeeSourcesChanged, qontrol::UNIQUE);
}

void AppSettings::onFeeSourcesChanged() {
    auto selected = m_host->selectedFees();
    auto preferences = selected.preferences;
    m_fee_default_label->setText(feeRecommendation(selected.default_rate));
    m_fee_low_label->setText(feeRecommendation(selected.low));
    m_fee_medium_label->setText(feeRecommendation(selected.medium));
    m_fee_fast_label->setText(feeRecommendation(selected.fast));
    m_fee_source_picker->clear();
    m_fee_source_picker->addItem(TR("settings-price-none"), QString());
    bool hasDefault = false;
    bool hasSmart = false;
    for (const auto &source : m_host->feeSources()) {
        if (source.has_default || source.has_smart) {
            m_fee_source_picker->addItem(source.name, source.id);
        }
        if (source.has_default) {
            hasDefault = true;
        }
        if (source.has_smart) {
            hasSmart = true;
        }
    }
    if (!preferences.source.isEmpty() && m_fee_source_picker->findData(preferences.source) < 0) {
        m_fee_source_picker->addItem(TR("settings-price-unavailable").arg(preferences.source),
                                     preferences.source);
    }
    m_fee_source_picker->setCurrentIndex(m_fee_source_picker->findData(preferences.source));
    m_fee_mode_picker->clear();
    m_fee_mode_picker->addItem(TR("fee-manual"), static_cast<int>(FeeMode::Manual));
    if (hasDefault || preferences.mode == FeeMode::Default) {
        m_fee_mode_picker->addItem(TR("fee-default"), static_cast<int>(FeeMode::Default));
    }
    if (hasSmart || preferences.mode == FeeMode::Smart) {
        m_fee_mode_picker->addItem(TR("fee-smart"), static_cast<int>(FeeMode::Smart));
    }
    m_fee_mode_picker->setCurrentIndex(
        m_fee_mode_picker->findData(static_cast<int>(preferences.mode)));
}

void AppSettings::onFeePreferencesActivated([[maybe_unused]] int index) {
    m_host->selectFees({.mode = static_cast<FeeMode>(m_fee_mode_picker->currentData().toInt()),
                        .source = m_fee_source_picker->currentData().toString()});
}

auto AppSettings::buildPlugins() -> QWidget * {
    m_plugins = foldable(QString());
    m_plugins->setContent((new qontrol::Column)
                              ->spacing(resolve(Spacing::S))
                              ->push(m_removal_notice)
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
    QString error = m_plugin_registry->pluginStopError(id);
    if (error.isEmpty()) {
        error = m_plugin_registry->pluginLoadError(id);
    }
    bool failed = !error.isEmpty();
    bool unloading = id == m_unloading_id || m_plugin_registry->pluginStopping(id);

    auto *row = new catalog::SelectRow;
    row->setIcon(failed ? icon::circleX : icon::puzzle);
    row->setTone(failed ? catalog::SelectRow::Tone::Error : catalog::SelectRow::Tone::Default);
    row->setTitle(m_plugin_registry->pluginName(id));
    auto description =
        unloading ? TR("settings-plugin-stopping") : m_plugin_registry->pluginDescription(id);
    row->setSub(failed ? error : description);
    row->setMeta(meta);
    auto *actions = (new qontrol::Row)->spacing(resolve(Spacing::S));
    if (m_plugin_registry->pluginDisableRetryable(id)) {
        auto *retryBtn = catalog::btnRetryDisablePlugin();
        retryBtn->setProperty(K_PLUGIN_ID, id);
        connect(retryBtn, &QPushButton::clicked, this, &AppSettings::onPluginDisableRetryClicked,
                qontrol::UNIQUE);
        actions->push(retryBtn);
    }
    if (!unloading && m_plugin_registry->pluginHasAccessHistory(id)) {
        // What it may use, once it has asked for something.
        auto *accessBtn = catalog::btnPluginAccess();
        accessBtn->setObjectName(QStringLiteral("pluginAccess"));
        accessBtn->setProperty(K_PLUGIN_ID, id);
        connect(accessBtn, &QPushButton::clicked, this, &AppSettings::onPluginAccessClicked,
                qontrol::UNIQUE);
        actions->push(accessBtn);
    }
    auto *removeBtn = catalog::btnRemovePlugin();
    removeBtn->setProperty(K_PLUGIN_ID, id);
    removeBtn->setEnabled(!m_plugin_registry->pluginStopPending(id));
    connect(removeBtn, &QPushButton::clicked, this, &AppSettings::onPluginRemoveClicked,
            qontrol::UNIQUE);
    actions->push(removeBtn);
    row->setStatus(actions);
    // Kept when empty, so the meta lines up on every row.
    row->setTrailingWidth(metric::TOGGLE_WIDTH);
    row->setChevron(false);
    row->setClickable(false);
    // A plugin that failed to load has nothing to switch.
    if (!failed) {
        auto *toggle = new catalog::Toggle;
        toggle->setChecked(m_plugin_registry->isPluginEnabled(id));
        toggle->setEnabled(!unloading && !m_plugin_registry->pluginStateLoading() &&
                           !m_plugin_registry->pluginEnablePending(id));
        toggle->setProperty(K_PLUGIN_ID, id);
        connect(toggle, &QCheckBox::clicked, this, &AppSettings::onPluginToggled, qontrol::UNIQUE);
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
    m_selected_theme = activeTheme;
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
    m_selected_theme = LIGHT;
    m_pending_palette.clear();
    m_pending_theme_to_apply.clear();
    if (auto *theme = Theme::get(); theme != nullptr) {
        theme->setMode(ThemeMode::Light);
        theme->setPalette(Palette::light());
        theme->apply();
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
    m_fees->header()->setLabel(TR("settings-fees"));
    m_fee_mode_field->setFieldLabel(m_fee_mode_picker, TR("settings-fee-mode"));
    m_fee_source_field->setFieldLabel(m_fee_source_picker, TR("settings-fee-source"));
    m_fee_recommendations->setFieldLabel(m_fee_default_label, TR("fee-default"));
    m_fee_recommendations->setFieldLabel(m_fee_low_label, TR("fee-low"));
    m_fee_recommendations->setFieldLabel(m_fee_medium_label, TR("fee-medium"));
    m_fee_recommendations->setFieldLabel(m_fee_fast_label, TR("fee-fast"));
    onFeeSourcesChanged();
    m_prices->header()->setLabel(TR("settings-prices"));
    m_price_fields->setFieldLabel(m_price_source_picker, TR("settings-price-source"));
    m_price_fields->setFieldLabel(m_price_currency_picker, TR("settings-price-currency"));
    onPriceSourcesChanged();
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
    m_removing_id.clear();
    m_disabling_id.clear();
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
    m_selected_theme = themeName;
    m_pending_palette.clear();
    m_pending_theme_to_apply.clear();

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
    if (m_plugin_registry != nullptr && m_plugin_registry->pluginRemovalReadError().isEmpty() &&
        m_flow == nullptr && m_import_worker == nullptr && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

// Each dropped bundle is sealed into the state off the GUI thread: the plugin is
// listed, off, until turned on.
void AppSettings::dropEvent(QDropEvent *event) {
    if (m_flow != nullptr || m_import_worker != nullptr || m_plugin_registry == nullptr ||
        !m_plugin_registry->pluginRemovalReadError().isEmpty() || !event->mimeData()->hasUrls()) {
        return;
    }
    event->acceptProposedAction();
    m_plugin_notice->setVisible(false);
    m_plugin_result->setVisible(false);
    QStringList paths;
    for (const auto &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    if (paths.isEmpty()) {
        return;
    }
    m_import_worker = QThread::create([this, paths] {
        QStringList ids;
        QStringList errors;
        for (const auto &path : paths) {
            auto result = installPluginBundle(path);
            if (result.is_ok) {
                ids.append(QString::fromUtf8(result.plugin_id.data(), result.plugin_id.size()));
            } else {
                errors.append(QString::fromUtf8(result.error.data(), result.error.size()));
            }
        }
        emit bundlesImported(ids, errors);
    });
    connect(m_import_worker, &QThread::finished, m_import_worker, &QThread::deleteLater,
            qontrol::UNIQUE);
    m_import_worker->start();
}

void AppSettings::onBundlesImported(const QStringList &plugin_ids, const QStringList &errors) {
    if (m_plugin_registry == nullptr) {
        return;
    }
    if (!errors.isEmpty()) {
        m_plugin_notice->setTitle(TR("settings-plugin-add-failed"));
        m_plugin_notice->setText(errors.join(QLatin1Char('\n')));
        m_plugin_notice->setVisible(true);
    }
    QStringList added;
    QStringList updated;
    for (const auto &id : plugin_ids) {
        (m_plugin_registry->isPluginEnabled(id) ? updated : added).append(id);
    }
    m_plugin_registry->rescanDiscoveredPlugins();
    if (!m_plugin_registry->pluginRemovalReadError().isEmpty()) {
        return;
    }
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
    if (toggle == nullptr || m_plugin_registry == nullptr ||
        !m_plugin_registry->pluginRemovalReadError().isEmpty()) {
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
    if (sender() == nullptr || m_plugin_registry == nullptr ||
        !m_plugin_registry->pluginRemovalReadError().isEmpty()) {
        return;
    }
    m_plugin_notice->setVisible(false);
    m_plugin_result->setVisible(false);
    openFlow(new AccessFlow(m_plugin_registry, sender()->property(K_PLUGIN_ID).toString()));
}

void AppSettings::onPluginRemoveClicked() {
    if (sender() == nullptr || m_plugin_registry == nullptr ||
        !m_plugin_registry->pluginRemovalReadError().isEmpty()) {
        return;
    }
    m_removing_id = sender()->property(K_PLUGIN_ID).toString();
    openFlow(new RemoveFlow(m_plugin_registry->pluginName(m_removing_id)));
}

void AppSettings::onPluginDisableRetryClicked() {
    if (sender() == nullptr || m_plugin_registry == nullptr ||
        !m_plugin_registry->pluginRemovalReadError().isEmpty()) {
        return;
    }
    requestPluginDisable(sender()->property(K_PLUGIN_ID).toString());
}

void AppSettings::onPluginStopFinished(const QString &plugin_id, bool removed,
                                       const QString &error) {
    m_unloading_id.clear();
    if (m_stopping_theme_plugins.remove(plugin_id) &&
        (error.isEmpty() || m_plugin_registry->pluginStopping(plugin_id))) {
        switchToLightTheme();
    }
    m_plugin_notice->setVisible(!error.isEmpty());
    m_plugin_result->setVisible(error.isEmpty());
    if (!error.isEmpty()) {
        m_plugin_notice->setTitle(TR("settings-plugin-stop-failed"));
        m_plugin_notice->setText(error);
    } else {
        m_plugin_result->setText(
            (removed ? TR("settings-plugin-removed") : TR("settings-plugin-disabled"))
                .arg(plugin_id));
    }
    refreshThemeProviders();
    requestThemeList();
    onPluginsChanged();
}

void AppSettings::onPluginsChanged() {
    auto error =
        m_plugin_registry == nullptr ? QString() : m_plugin_registry->pluginRemovalReadError();
    m_removal_notice->setTitle(TR("settings-plugin-state-read-failed"));
    m_removal_notice->setText(TR("settings-plugin-state-read-failed-text").arg(error));
    m_removal_notice->setVisible(!error.isEmpty());
    m_removal_retry_btn->setVisible(!error.isEmpty());
    m_plugins_column->setEnabled(error.isEmpty());
    bool reading = m_plugin_registry != nullptr && m_plugin_registry->pluginStateLoading();
    m_plugin_drop->setEnabled(error.isEmpty() && !reading);
    setAcceptDrops(error.isEmpty() && !reading);
    if (!error.isEmpty()) {
        if (m_flow != nullptr) {
            m_flow->setEnabled(false);
            showMain();
            closeFlow();
        }
        m_plugin_notice->setVisible(false);
        m_plugin_result->setVisible(false);
    }
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
            if (id.isEmpty() || m_plugin_registry->isPluginBuiltin(id) ||
                m_plugin_registry->pluginHasBuiltinModules(id)) {
                continue;
            }
            // External row text must not retain QStringLiteral backing in a library
            // that will be unmapped before the next completion-driven refresh.
            meta = m_plugin_registry->pluginMetadata(id);
            id = meta.value(QStringLiteral("id"));
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

void AppSettings::onRetryPluginState() {
    if (m_plugin_registry != nullptr) {
        m_plugin_registry->reloadEnabledPlugins();
    }
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
    auto themeOwner = m_theme_plugin.value(m_selected_theme);
    if (m_stopping_theme_plugins.remove(themeOwner)) {
        switchToLightTheme();
    }
    refreshThemeProviders();
    requestThemeList();
    applyPersistedTheme();
}

void AppSettings::onThemeNames(ReqId req_id, const QStringList &themes) {
    if (m_pending_theme_list.value(req_id, nullptr) != qobject_cast<IThemeProvider *>(sender())) {
        return;
    }
    auto *provider = m_pending_theme_list.take(req_id);
    if (provider == nullptr) {
        return;
    }

    for (const auto &themeNameRaw : themes) {
        QString themeName = QString::fromUtf8(themeNameRaw.trimmed().toUtf8());
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
    if (!m_pending_palette.contains(req_id) ||
        m_pending_palette.value(req_id) != qobject_cast<IThemeProvider *>(sender())) {
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
    m_removing_id.clear();
    // A Back from disabling leaves the plugin enabled: its toggle shows it again.
    onPluginsChanged();
    showMain();
    closeFlow();
}

void AppSettings::onFlowConfirmed() {
    if (m_plugin_registry == nullptr || !m_plugin_registry->pluginRemovalReadError().isEmpty()) {
        m_removing_id.clear();
        m_disabling_id.clear();
        showMain();
        closeFlow();
        onPluginsChanged();
        return;
    }
    if (!m_removing_id.isEmpty()) {
        auto id = m_removing_id;
        if (m_theme_plugin.value(m_selected_theme) == id) {
            m_stopping_theme_plugins.insert(id);
        }
        m_removing_id.clear();
        showMain();
        closeFlow();
        m_plugin_notice->setVisible(false);
        m_plugin_result->setText(TR("settings-plugin-removing").arg(id));
        m_plugin_result->setVisible(true);
        m_plugin_registry->removePlugin(id);
        return;
    }
    QString id = m_disabling_id;
    showMain();
    closeFlow();
    requestPluginDisable(id);
}

void AppSettings::requestPluginDisable(const QString &id) {
    auto themeGoes = m_theme_plugin.value(m_selected_theme) == id;
    if (themeGoes) {
        m_stopping_theme_plugins.insert(id);
    }
    m_unloading_id = id;
    m_plugin_notice->setVisible(false);
    m_plugin_result->setVisible(false);
    if (!m_plugin_registry->setEnabled(id, false)) {
        m_unloading_id.clear();
        if (themeGoes) {
            m_stopping_theme_plugins.remove(id);
        }
        if (!m_plugin_notice->isVisible()) {
            m_plugin_notice->setTitle(
                TR("settings-disable-refused").arg(m_plugin_registry->pluginName(id)));
            m_plugin_notice->setText(TR("settings-disable-refused-text"));
            m_plugin_notice->setVisible(true);
        }
        onPluginsChanged();
    }
}

void AppSettings::onPriceSourcesChanged() {
    auto sources = m_host->priceSources();
    auto selected = m_host->selectedPrice();
    m_prices->setVisible(!sources.empty());
    m_prices_separator->setVisible(!sources.empty());
    m_price_source_picker->clear();
    m_price_source_picker->addItem(TR("settings-price-none"), QString());
    QStringList currencies;
    for (const auto &source : sources) {
        m_price_source_picker->addItem(source.name, source.id);
        if (source.id == selected.source) {
            currencies = source.currencies;
        }
    }
    if (!selected.source.isEmpty() && m_price_source_picker->findData(selected.source) < 0) {
        m_price_source_picker->addItem(TR("settings-price-unavailable").arg(selected.source),
                                       selected.source);
    }
    m_price_source_picker->setCurrentIndex(m_price_source_picker->findData(selected.source));
    m_price_currency_picker->clear();
    m_price_currency_picker->addItem(TR("settings-price-none"), QString());
    for (const auto &currency : currencies) {
        m_price_currency_picker->addItem(currency, currency);
    }
    if (!selected.currency.isEmpty() && !currencies.contains(selected.currency)) {
        m_price_currency_picker->addItem(TR("settings-price-unavailable").arg(selected.currency),
                                         selected.currency);
    }
    m_price_currency_picker->setCurrentIndex(m_price_currency_picker->findData(selected.currency));
    m_price_currency_picker->setEnabled(!currencies.empty());
    // Repopulating the combo can shrink its layout before Qt updates the content hint.
    m_price_source_picker->setMinimumWidth(
        std::max(metric::SETTINGS_PICKER_WIDTH, m_price_source_picker->sizeHint().width()));
}

void AppSettings::onPriceSourceActivated([[maybe_unused]] int index) {
    m_host->selectPrice(m_price_source_picker->currentData().toString(), QString());
}

void AppSettings::onPriceCurrencyActivated([[maybe_unused]] int index) {
    m_host->selectPrice(m_price_source_picker->currentData().toString(),
                        m_price_currency_picker->currentData().toString());
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
