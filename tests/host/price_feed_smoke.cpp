#include "PriceProbe.h"

#include "MainWindow.h"
#include "account/views/Send.h"
#include "catalog/containers/Tabs.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/Input.h"
#include "catalog/panels/send/TxFlow.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "theme/Theme.h"
#include "views/modals/AppSettings.h"
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QVariant>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <limits>
#include <string>
#include <thread>

#ifdef SILENT_PRICE_FEED_FIXTURE
#include "host/PluginLibrary.h"
#include <QFile>
#include <cerrno>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace smoke {
namespace {

class PriceSend final : public account::view::Send {
public:
    using Send::Send;

    auto transaction() -> std::optional<TransactionTemplate> {
        auto tx = txTemplate();
        if (!tx.has_value()) {
            qFatal("price probe expected a valid transaction template");
        }
        return tx;
    }
};

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("price feed smoke: %s", message);
    }
}

auto input(account::view::Send &send, const QString &placeholder) -> QLineEdit * {
    for (auto *field : send.findChildren<QLineEdit *>()) {
        if (field->placeholderText() == placeholder) {
            return field;
        }
    }
    qFatal("price probe input missing");
}

void edit(QLineEdit *field, const QString &text) {
    field->selectAll();
    QKeyEvent event(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, text);
    QApplication::sendEvent(field, &event);
}

auto hasLabel(QWidget &widget, const QString &text) -> bool {
    return std::ranges::any_of(widget.findChildren<catalog::Label *>(), [&text](auto *label) {
        return label->text() == text && !label->isHidden();
    });
}

void hover(account::view::Send &send, double x, double y, const QString &title,
           const QString &fiat) {
    auto *flow = send.findChild<catalog::TxFlow *>();
    require(flow != nullptr, "real transaction flow exists");
    auto rendered = flow->grab();
    require(!rendered.isNull(), "flow paints its current geometry offscreen");
    // TxFlow's 300-wide canvas occupies the middle 70% of its widget; y is unscaled.
    QPointF point(flow->width() * (0.15 + x * 0.7 / 300), y);
    QMouseEvent event(QEvent::MouseMove, point, point, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(flow, &event);
    if (!hasLabel(send, title)) {
        qFatal("hover card missing '%s' at %.0f,%.0f", qPrintable(title), x, y);
    }
    require(fiat.isEmpty() || hasLabel(send, fiat), "hover card uses cached selected fiat");
    if (fiat.isEmpty()) {
        for (auto *label : send.findChildren<catalog::Label *>()) {
            require(label->isHidden() || !label->text().startsWith(QStringLiteral("~ ")),
                    "unavailable quote leaves no displayed hover fiat");
        }
    }
}

void registrationReuse(HostImpl *host, PriceModule *module) {
    PriceFeed feed;
    feed.offered = {QStringLiteral("EUR")};
    PriceInstance instance;
    instance.instanceId = QStringLiteral("price-reused-object");
    instance.provider = &feed;
    host->registerInstance(instance.id(), &instance, module);
    auto sourceId = host->priceSources().first().id;
    host->selectPrice(sourceId, QStringLiteral("EUR"));
    require(host->selectedPrice().rate == 50000, "reuse probe starts with its initial quote");

    feed.offered = {QStringLiteral("USD")};
    std::thread stalePush([&feed]() {
        feed.publishCurrencies();
        feed.publish(QStringLiteral("EUR"), 12345);
    });
    stalePush.join();
    host->removeInstance(instance.id(), module);
    feed.offered = {QStringLiteral("EUR")};
    feed.value = 70000;
    host->registerInstance(instance.id(), &instance, module);
    QApplication::processEvents();
    require(host->selectedPrice().rate == 70000 && feed.requests == 2 &&
                host->priceSources().first().currencies == QStringList{QStringLiteral("EUR")},
            "old queued currencies and rates cannot affect the same re-registered QObject");

    QChar currencyStorage[]{QChar('E'), QChar('U'), QChar('R')};
    auto borrowedCurrency = QString::fromRawData(currencyStorage, 3);
    feed.offered = {borrowedCurrency};
    std::thread ownedPush([&feed, &borrowedCurrency]() {
        feed.publishCurrencies();
        feed.publish(borrowedCurrency, 80000);
    });
    ownedPush.join();
    currencyStorage[0] = QChar('X');
    currencyStorage[1] = QChar('X');
    currencyStorage[2] = QChar('X');
    QApplication::processEvents();
    require(host->selectedPrice().rate == 80000 &&
                host->priceSources().first().currencies == QStringList{QStringLiteral("EUR")},
            "queued live payloads own the list and currency strings before dispatch");
    host->removeInstance(instance.id(), module);
}

void priceSourceGeometry(HostImpl *host, const QString &first_id, const QString &second_id) {
    Theme::get()->apply();
    MainWindow window;
    auto *settings = window.findChild<modal::AppSettings *>();
    require(settings != nullptr, "real window has global settings");
    window.findChild<catalog::Tabs *>()->setCurrentWidget(settings);
    QComboBox *source = nullptr;
    for (auto *picker : settings->findChildren<QComboBox *>()) {
        if (picker->findData(first_id) >= 0) {
            source = picker;
        }
    }
    require(source != nullptr, "real window has a source picker");
    auto selected = host->selectedPrice();
    auto minimumWidth = window.minimumWidth();
    window.resize(minimumWidth, window.height());
    window.show();
    for (const auto &id : {QString(), first_id, second_id}) {
        auto index = source->findData(id);
        require(index >= 0, "all source choices remain available");
        source->setCurrentIndex(index);
        emit source->activated(index);
        QApplication::processEvents();
        require(window.width() == minimumWidth, "source content does not enlarge minimum window");
        QStyleOptionComboBox option;
        option.initFrom(source);
        option.rect = source->rect();
        option.currentText = source->currentText();
        auto textRect = source->style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                        QStyle::SC_ComboBoxEditField, source);
        for (int row = 0; row < source->count(); ++row) {
            require(source->fontMetrics().horizontalAdvance(source->itemText(row)) <=
                        textRect.width(),
                    "entire module and instance label fits even before that source is selected");
        }
        auto *ancestor = source->parentWidget();
        while (ancestor != nullptr && qobject_cast<QAbstractScrollArea *>(ancestor) == nullptr) {
            ancestor = ancestor->parentWidget();
        }
        auto *scroll = qobject_cast<QAbstractScrollArea *>(ancestor);
        require(scroll != nullptr &&
                    scroll->viewport()->rect().contains(
                        QRect(source->mapTo(scroll->viewport(), QPoint()), source->size())),
                "complete source control stays inside the settings viewport");
        source->showPopup();
        QApplication::processEvents();
        auto *view = source->view();
        for (int row = 0; row < source->count(); ++row) {
            auto rowRect = view->visualRect(source->model()->index(row, source->modelColumn()));
            require(rowRect.width() >= view->sizeHintForColumn(source->modelColumn()),
                    "source popup rows preserve the complete distinguishing label");
        }
        source->hidePopup();
    }
    require(source->itemText(source->findData(first_id)) ==
                    QStringLiteral("Price probe (price-one)") &&
                source->itemText(source->findData(second_id)) ==
                    QStringLiteral("Price probe (price-two)"),
            "source labels retain both module and distinct instance names");
    host->selectPrice(selected.source, selected.currency);
}

#ifdef SILENT_PRICE_FEED_FIXTURE
auto literalPage(IFeed *feed, std::uintptr_t page_size) -> void * {
    auto address = static_cast<std::uintptr_t>(feed->raw(QByteArrayLiteral("literal-address")));
    // mincore needs the actual plugin storage address to prove dlclose unmapped it.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<void *>(address - address % page_size);
}

void libraryUnload(HostImpl *host, PriceModule *module) {
    QFile file(QString::fromUtf8(SILENT_PRICE_FEED_FIXTURE));
    require(file.open(QIODevice::ReadOnly), "test price library opens");
    auto bytes = file.readAll();
    PluginLibrary library;
    require(library.load(bytes, QStringLiteral("price-feed-fixture.so")), "test library loads");
    auto *feed = dynamic_cast<IFeed *>(library.instance());
    require(feed != nullptr, "test library exports its real feed QObject");
    auto pageSize = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
    auto *page = literalPage(feed, pageSize);
    unsigned char residency = 0;
    require(::mincore(page, pageSize, &residency) == 0, "plugin literal storage starts mapped");
    PriceInstance instance;
    instance.instanceId = QStringLiteral("price-unload-library");
    instance.provider = feed;
    host->registerInstance(instance.id(), &instance, module);
    auto sourceId = host->priceSources().first().id;
    host->selectPrice(sourceId, QStringLiteral("EUR"));
    require(host->selectedPrice().rate == 50000, "library's initial quote is cached");
    feed->raw(QByteArrayLiteral("queue"));
    host->removeInstance(instance.id(), module);
    require(library.unload(), "library unload succeeds with queued currencies and rates");
    errno = 0;
    require(::mincore(page, pageSize, &residency) == -1 && errno == ENOMEM,
            "the library's literal storage really is unmapped, not kept resident by dlclose");

    require(library.load(bytes, QStringLiteral("price-feed-fixture.so")), "test library reloads");
    feed = dynamic_cast<IFeed *>(library.instance());
    require(feed != nullptr, "reloaded feed exists");
    instance.provider = feed;
    host->registerInstance(instance.id(), &instance, module);
    QApplication::processEvents();
    require(host->selectedPrice().rate == 50000 && feed->raw(QByteArrayLiteral("requests")) == 1 &&
                host->priceSources().first().currencies == QStringList{QStringLiteral("EUR")},
            "queued payloads survive unmapping and cannot update or request from the reload");
    page = literalPage(feed, pageSize);
    feed->raw(QByteArrayLiteral("queue"));
    host->removeInstance(instance.id(), module);
    require(library.unload(), "reloaded test library unloads");
    errno = 0;
    require(::mincore(page, pageSize, &residency) == -1 && errno == ENOMEM,
            "reloaded literal storage is unmapped before queued payload destruction");
    QApplication::processEvents();
    require(!host->selectedPrice().rate.has_value() && host->priceSources().empty(),
            "queued payload destruction after unload leaves the source unavailable");
}
#endif

} // namespace

auto priceFeedSmoke() -> bool {
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    host->selectPrice({}, {});
    PriceEvents events;
    QObject::connect(host->events(), &HostEvents::priceSourcesChanged, &events,
                     &PriceEvents::onSourcesChanged, qontrol::UNIQUE);
    QObject::connect(host->events(), &HostEvents::selectedPriceChanged, &events,
                     &PriceEvents::onSelectedChanged, qontrol::UNIQUE);
    require(!host->selectedPrice().rate.has_value(), "empty selection has no price");
    PricePlugin plugin;
    PluginRegistry registry;
    registry.registerBuiltin(&plugin);
    PriceFeed first;
    PriceInstance one;
    one.instanceId = QStringLiteral("price-one");
    one.provider = &first;
    host->registerInstance(one.id(), &one, &plugin.module);
    require(first.discoveries == 1 && first.requests == 1,
            "registration separately discovers currencies and requests rates once");
    require(first.requested == QStringList({QStringLiteral("EUR"), QStringLiteral("USD")}),
            "initial request includes every offered currency");
    require(host->priceSources().size() == 1, "one price provider discovered");
    require(events.sources == 1 && events.selected == 0,
            "discovery notifies sources without selecting a quote");
    require(host->selectedPrice().source.isEmpty(), "discovery must not select a provider");
    auto firstId = host->priceSources().first().id;
    require(firstId == QStringLiteral("[\"price.plugin\",\"price.module\",\"price-one\"]"),
            "source identity is stable and namespaced");
    host->selectPrice(firstId, QStringLiteral("EUR"));
    require(host->selectedPrice().rate == 50000, "selected initial rate is cached");
    require(QString::fromStdString(std::string(::app_price_source())) == firstId &&
                std::string(::app_price_currency()) == "EUR",
            "selection persists globally");
    HostImpl reopened;
    require(reopened.selectedPrice().source == firstId &&
                reopened.selectedPrice().currency == QStringLiteral("EUR") &&
                !reopened.selectedPrice().rate.has_value(),
            "new host restores selection without inventing a cached rate");

    PriceFeed second;
    second.deferred = true;
    PriceInstance two;
    two.instanceId = QStringLiteral("price-two");
    two.provider = &second;
    host->registerInstance(two.id(), &two, &plugin.module);
    require(second.discoveries == 1 && second.requests == 0,
            "late async feed waits for currency discovery before requesting rates");
    second.publishCurrencies();
    require(second.requests == 1 && host->priceSources().size() == 2,
            "async currency discovery requests initial rates");
    second.publish(QStringLiteral("EUR"), 90000);
    require(host->selectedPrice().rate == 50000,
            "same-currency sources cannot overwrite each other");
    first.publish(QStringLiteral("EUR"), 60000);
    require(host->selectedPrice().rate == 60000, "selected pushes update cache");
    auto notifications = events.selected;
    first.publish(QStringLiteral("EUR"), 60000);
    second.publish(QStringLiteral("EUR"), 90000);
    require(events.selected == notifications,
            "unchanged or unselected quotes do not notify selection");
    first.publish(QStringLiteral("EUR"), 0);
    require(!host->selectedPrice().rate.has_value(), "withdrawal does not fall back");
    for (double invalid : {-1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        first.publish(QStringLiteral("EUR"), invalid);
        require(!host->selectedPrice().rate.has_value(), "invalid quote is unavailable");
    }
    first.publish(QStringLiteral("EUR"), 50000);

    Theme::init();
    priceSourceGeometry(host, firstId,
                        QStringLiteral("[\"price.plugin\",\"price.module\",\"price-two\"]"));
    {
        modal::AppSettings settings;
        QComboBox *sourcePicker = nullptr;
        for (auto *picker : settings.findChildren<QComboBox *>()) {
            if (picker->findData(firstId) >= 0) {
                sourcePicker = picker;
            }
        }
        require(sourcePicker != nullptr && sourcePicker->isVisibleTo(&settings),
                "global price section is present");
        PriceBackend backend;
        PriceSend send(&backend);
        auto *btc = input(send, TR("send-placeholder-amount"));
        auto *fiat = input(send, QStringLiteral("0.00"));
        btc->setText(QStringLiteral("0.01000000"));
        require(send.transaction()->outputs[0].amount == 1000000,
                "BTC input creates a payment of exactly one million sats");
        require(fiat->text() == QStringLiteral("500.00"), "recipient displays selected fiat");
        require(hasLabel(send, QStringLiteral("~ 500.00 EUR")), "summary uses currency code");
        auto simulations = backend.simulations;
        auto signingUpdates = backend.signing->updates;
        first.publish(QStringLiteral("EUR"), 100000);
        require(btc->text() == QStringLiteral("0.01000000") &&
                    fiat->text() == QStringLiteral("1000.00"),
                "price changes fiat, not BTC");
        require(backend.simulations == simulations && backend.signing->updates == signingUpdates,
                "price notifications never simulate or replace the transaction");
        require(send.transaction()->outputs[0].amount == 1000000 &&
                    send.transaction()->fee_rate == 1,
                "price notification leaves the actual payment and fee template unchanged");
        edit(fiat, QStringLiteral("2000"));
        require(btc->text() == QStringLiteral("0.02000000"), "explicit fiat edit changes BTC");
        require(send.transaction()->outputs[0].amount == 2000000,
                "explicit fiat edit creates exactly two million sats at the current rate");
        first.publish(QStringLiteral("EUR"), 50000);
        require(btc->text() == QStringLiteral("0.02000000") &&
                    fiat->text() == QStringLiteral("1000.00"),
                "originally fiat-entered payment stays at its BTC amount");
        require(send.transaction()->outputs[0].amount == 2000000,
                "fiat-origin payment keeps two million sats after rate update");
        auto secondId = QStringLiteral("[\"price.plugin\",\"price.module\",\"price-two\"]");
        second.publish(QStringLiteral("USD"), 80000);
        int sourceIndex = sourcePicker->findData(secondId);
        sourcePicker->setCurrentIndex(sourceIndex);
        emit sourcePicker->activated(sourceIndex);
        require(host->selectedPrice().source == secondId &&
                    host->selectedPrice().currency.isEmpty(),
                "user source selection does not auto-select a currency");
        QComboBox *currencyPicker = nullptr;
        for (auto *picker : settings.findChildren<QComboBox *>()) {
            if (picker->findData(QStringLiteral("USD")) >= 0) {
                currencyPicker = picker;
            }
        }
        require(currencyPicker != nullptr, "selected source offers currency choices");
        int currencyIndex = currencyPicker->findData(QStringLiteral("USD"));
        currencyPicker->setCurrentIndex(currencyIndex);
        emit currencyPicker->activated(currencyIndex);
        require(btc->text() == QStringLiteral("0.02000000") &&
                    fiat->text() == QStringLiteral("1600.00") &&
                    hasLabel(send, QStringLiteral("USD")),
                "source/currency changes keep BTC fixed");
        require(send.transaction()->outputs[0].amount == 2000000,
                "source/currency changes preserve the payment's satoshi amount");
        host->selectPrice({}, {});
        require(fiat->parentWidget()->isHidden(),
                "no selection disables fiat even with live providers");
        host->selectPrice(firstId, QStringLiteral("EUR"));
        fiat->setText(QStringLiteral("9999"));
        send.onProcess();
        require(btc->text() == QStringLiteral("0.02000000"),
                "programmatic fiat edits and unrelated processing do not convert BTC");
        first.publish(QStringLiteral("EUR"), 0);
        require(fiat->parentWidget()->isHidden() && fiat->text().isEmpty(),
                "unavailable price hides and clears recipient fiat");
        require(!hasLabel(send, QStringLiteral("~ 1000.00 EUR")), "unavailable summary hides fiat");
        first.publish(QStringLiteral("EUR"), 50000);
        emit backend.signing->lockedChanged(true);
        first.publish(QStringLiteral("EUR"), 100000);
        require(btc->text() == QStringLiteral("0.02000000") &&
                    fiat->text() == QStringLiteral("2000.00") && !fiat->isEnabled(),
                "locked payment refreshes fiat without allowing edits");
        emit backend.signing->lockedChanged(false);
        QCheckBox *max = nullptr;
        for (auto *checkbox : send.findChildren<QCheckBox *>()) {
            if (checkbox->property("outputId").isValid()) {
                max = checkbox;
            }
        }
        require(max != nullptr, "payment has MAX control");
        backend.validSimulation = true;
        max->setChecked(true);
        auto maxBtc = btc->text();
        require(maxBtc == QStringLiteral("0.09998000"), "MAX uses the simulation's output amount");
        simulations = backend.simulations;
        first.publish(QStringLiteral("EUR"), 50000);
        require(btc->text() == maxBtc && !fiat->isEnabled() && backend.simulations == simulations,
                "price update preserves MAX and does not enable fiat editing");
        require(send.transaction()->outputs[0].max && send.transaction()->outputs[0].amount == 0,
                "MAX stays a builder instruction, not a price-derived amount");
        backend.validSimulation = false;
        max->setChecked(false);

        SavedPsbtEntry saved{};
        Output payment{};
        payment.amount = 3000000;
        payment.address = rust::String("saved-recipient");
        saved.frozen_template.outputs.push_back(payment);
        saved.frozen_template.fee = 2000;
        SavedPsbtInput savedInput{};
        savedInput.value = 10000000;
        savedInput.outpoint = rust::String("saved-input:0");
        saved.inputs.push_back(savedInput);
        send.restoreSavedPsbt(saved);
        btc = input(send, TR("send-placeholder-amount"));
        fiat = input(send, QStringLiteral("0.00"));
        simulations = backend.simulations;
        first.publish(QStringLiteral("EUR"), 50000);
        require(btc->text() == QStringLiteral("0.03000000") &&
                    fiat->text() == QStringLiteral("1500.00") && !fiat->isEnabled(),
                "restored PSBT keeps frozen BTC and refreshes its fiat");
        require(hasLabel(send, QStringLiteral("~ 1.00 EUR")),
                "restored fee uses actual frozen fee");
        require(hasLabel(send, QStringLiteral("~ 1501.00 EUR")), "total includes frozen fee");
        require(hasLabel(send, QStringLiteral("~ 3499.00 EUR")),
                "change uses frozen inputs and outputs");
        require(backend.simulations == simulations, "restored PSBT never resimulates on price");
        require(send.transaction()->outputs[0].amount == 3000000 && send.transaction()->fee == 2000,
                "restored template retains its exact frozen payment and absolute fee");
        hover(send, 12, 125, TR("txflow-input-n").arg(1), QStringLiteral("~ 5000.00 EUR"));
        hover(send, 286, 45, TR("txflow-output-n").arg(1), QStringLiteral("~ 1500.00 EUR"));
        hover(send, 286, 150, TR("txflow-change-title"), QStringLiteral("~ 3499.00 EUR"));
        hover(send, 286, 233, TR("txflow-fee-title"), QStringLiteral("~ 1.00 EUR"));
        first.publish(QStringLiteral("EUR"), 0);
        hover(send, 12, 125, TR("txflow-input-n").arg(1), {});
        hover(send, 286, 45, TR("txflow-output-n").arg(1), {});
        hover(send, 286, 150, TR("txflow-change-title"), {});
        hover(send, 286, 233, TR("txflow-fee-title"), {});
    }

    std::thread queuedPush([&first]() { first.publish(QStringLiteral("EUR"), 12345); });
    queuedPush.join();
    host->removeInstance(one.id(), &plugin.module);
    require(one.stopped && !host->selectedPrice().rate.has_value() &&
                host->selectedPrice().source == firstId,
            "removal invalidates cache but retains selection");
    first.publish(QStringLiteral("EUR"), 12345);
    require(!host->selectedPrice().rate.has_value(), "removed feed cannot push stale rates");
    PriceFeed replacement;
    replacement.value = 70000;
    one.provider = &replacement;
    host->registerInstance(one.id(), &one, &plugin.module);
    QApplication::processEvents();
    require(host->selectedPrice().source == firstId && host->selectedPrice().rate == 70000,
            "reload recovers same stable selection with fresh quotes");
    first.publish(QStringLiteral("EUR"), 12345);
    require(host->selectedPrice().rate == 70000, "old plugin generation cannot update replacement");
    replacement.offered = {QStringLiteral("USD")};
    replacement.publishCurrencies();
    require(!host->selectedPrice().rate.has_value(), "withdrawn currency invalidates quote");
    replacement.offered = {QStringLiteral("EUR")};
    replacement.publishCurrencies();
    require(host->selectedPrice().rate == 70000, "currency can return through fresh request");
    QChar currencyStorage[]{QChar('E'), QChar('U'), QChar('R')};
    auto borrowedCurrency = QString::fromRawData(currencyStorage, 3);
    replacement.offered = {borrowedCurrency};
    replacement.publishCurrencies();
    host->selectPrice(firstId, borrowedCurrency);
    replacement.publish(borrowedCurrency, 75000);
    currencyStorage[0] = QChar('X');
    currencyStorage[1] = QChar('X');
    currencyStorage[2] = QChar('X');
    require(host->selectedPrice().currency == QStringLiteral("EUR") &&
                host->selectedPrice().rate == 75000 &&
                host->priceSources().first().currencies == QStringList{QStringLiteral("EUR")},
            "cached currency lists, rate keys and selection own their plugin-provided strings");

    PriceInstance absent;
    absent.instanceId = QStringLiteral("price-null");
    host->registerInstance(absent.id(), &absent, &plugin.module);
    PriceFeed inert;
    inert.active = false;
    PriceInstance inactive;
    inactive.instanceId = QStringLiteral("price-inert");
    inactive.provider = &inert;
    host->registerInstance(inactive.id(), &inactive, &plugin.module);
    require(inert.discoveries == 0 && host->priceSources().size() == 2,
            "null and inert feeds never become price providers");
    host->removeInstance(absent.id(), &plugin.module);
    host->removeInstance(inactive.id(), &plugin.module);
    NonPriceFeed nonPrice;
    inactive.provider = &nonPrice;
    host->registerInstance(inactive.id(), &inactive, &plugin.module);
    require(host->priceSources().size() == 2, "implemented non-price feed defaults stay inert");
    host->removeInstance(inactive.id(), &plugin.module);
    host->removeInstance(one.id(), &plugin.module);
    host->removeInstance(two.id(), &plugin.module);
    require(host->priceSources().empty(), "all providers removed");
    {
        modal::AppSettings settings;
        for (auto *picker : settings.findChildren<QComboBox *>()) {
            if (picker->findData(firstId) >= 0) {
                require(!picker->isVisibleTo(&settings), "price section hides without providers");
            }
        }
    }
    auto *destroyedFeed = new PriceFeed;
    one.provider = destroyedFeed;
    host->registerInstance(one.id(), &one, &plugin.module);
    require(host->selectedPrice().rate == 50000, "live feed recovers selection");
    delete destroyedFeed;
    one.provider = nullptr;
    require(host->priceSources().empty() && !host->selectedPrice().rate.has_value(),
            "unexpected feed destruction invalidates source safely");
    host->removeInstance(one.id(), &plugin.module);
    QApplication::processEvents();
    require(first.discoveries == 1 && second.discoveries == 1 && second.requests == 1,
            "event processing never polls providers");
    registrationReuse(host, &plugin.module);
#ifdef SILENT_PRICE_FEED_FIXTURE
    libraryUnload(host, &plugin.module);
#endif
    host->selectPrice({}, {});
    require(std::string(::app_price_source()).empty() &&
                std::string(::app_price_currency()).empty(),
            "explicit none selection persists without defaults");
    return true;
}

} // namespace smoke
