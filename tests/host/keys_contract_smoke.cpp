#include "AppController.h"
#include "ExternalSigner.h"
#include "KeysBoundaryProbe.h"
#include "MainWindow.h"
#include "MockPlugin.h"
#include "account/Spending.h"
#include "account/TransactionJob.h"
#include "catalog/containers/Tabs.h"
#include "host/CallerHandles.h"
#include "host/Host.h"
#include "modules/hwi/HwiSigner.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/flow/signingflow/DeviceSign.h"
#include "theme/Theme.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <QToolButton>
#include <array>
#include <common.h>
#include <memory>

namespace smoke {
void keysMetadataSmoke(keys::KeysModule &module);

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("Keys contract: %s", message);
    }
}

void clickTab(QTabBar *bar, int index) {
    auto position = QPointF(bar->tabRect(index).center());
    QMouseEvent press(QEvent::MouseButtonPress, position, bar->mapToGlobal(position.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, position, bar->mapToGlobal(position.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &release);
}

void settingsGear(QTabBar *bar) {
    bool found = false;
    for (auto *button : bar->findChildren<QToolButton *>()) {
        if (button->isVisible() && !button->icon().isNull()) {
            found = true;
            require(button->parentWidget()->rect().contains(button->geometry()),
                    "Settings gear is not clipped by its tab holder");
        }
    }
    require(found, "Settings gear remains visible");
}

void selectedPage(catalog::Tabs *tabs, QTabBar *bar, QWidget *page, bool module) {
    require(tabs->currentWidget() == page, "the displayed page remains selected");
    require(bar->property("idle").toBool() == !module, "module-strip idle state matches the page");
    if (module) {
        require(bar->tabData(bar->currentIndex()).value<QObject *>() == page,
                "the highlighted module matches the displayed page");
    }
}

void operationRouting(keys::KeysModule &module, ExternalSigner &signer, quint32 mask) {
    struct Operation {
        QString name;
        quint32 required;
    };

    for (const auto &operation : {Operation{.name = QStringLiteral("ordinary"), .required = 1U},
                                  Operation{.name = QStringLiteral("taproot"), .required = 2U},
                                  Operation{.name = QStringLiteral("mixed"), .required = 3U}}) {
        QFile fixture(QDir(QStringLiteral(SILENT_KEYS_FIXTURES))
                          .filePath(QStringLiteral("signer-%1.hex").arg(operation.name)));
        require(fixture.open(QIODevice::ReadOnly), "operation PSBT fixture opens");
        auto psbt = QString::fromLatin1(fixture.readAll().trimmed());
        bool supported = (mask & operation.required) == operation.required;
        auto plan = module.rust()->signing_plan(psbt.toStdString());
        require(plan.is_ok && plan.candidates.size() == 1, "real PSBT produces one candidate");
        require(plan.candidates[0].can_sign == supported,
                "candidate checks the requested ordinary/Taproot operations");
        require(plan.candidates[0].route == (supported ? KeyRouteKind::Usb : KeyRouteKind::Unknown),
                "unsupported operations do not select the USB route");
        if (!supported) {
            require(plan.candidates[0].unusable == KeyUnusableReason::UnsupportedOperation,
                    "unsupported operation refusal is explicit");
        }
        SignalSpy failed;
        auto calls = signer.signCalls();
        auto *request = new keys::flow::DeviceSign(&module, &signer, QStringLiteral("external"),
                                                   psbt, signer.fingerprint());
        QObject::connect(request, &keys::flow::DeviceSign::failed, &failed, &SignalSpy::onSignal,
                         qontrol::UNIQUE);
        request->start(QString());
        require(signer.signCalls() - calls == (supported ? 1 : 0),
                "unsupported device operations are refused before dispatch");
        require(failed.count() == (supported ? 0 : 1), "device refusal emits its failure result");
        delete request;
    }
}

void signerCapabilities(keys::KeysModule &module) {
    require(plugin::SignerCapabilities{} ==
                plugin::SignerCapabilities{.ordinary = plugin::Support::Unsupported,
                                           .taproot = plugin::Support::Unsupported,
                                           .musig2 = plugin::Support::Unsupported,
                                           .silentPayments = plugin::Support::Unsupported},
            "omitted capabilities explicitly mean unsupported");
    auto network = keys::KeysModule::network();
    module.setNetwork(Network::Regtest);
    require(module.rust()->add_key("aabbccdd", "capability fixture").is_ok,
            "fixture key registers");
    require(module.rust()->add_route("aabbccdd", KeyRouteKind::Usb).is_ok,
            "fixture USB route registers");
    for (quint32 mask = 0; mask < 16; ++mask) {
        auto support = [mask](quint32 bit) {
            return (mask & bit) != 0 ? plugin::Support::Supported : plugin::Support::Unsupported;
        };
        plugin::SignerCapabilities expected{.ordinary = support(1),
                                            .taproot = support(2),
                                            .musig2 = support(8),
                                            .silentPayments = support(4)};
        ExternalSigner signer(expected);
        ISigner *publicSigner = &signer;
        require(publicSigner->capabilities() == expected,
                "public signer preserves all four capabilities");
        plugin::SignerEntry entry{.instanceId = QStringLiteral("external"), .signer = publicSigner};
        auto bits = keys::KeysModule::deviceCapabilities(entry);
        require(bits == mask, "Keys preserves all four non-HWI capability flags");
        auto device = keys::KeysModule::toLiveDevice(entry);
        require(device.capabilities == mask, "Keys passes all four capabilities to Rust");
        rust::Vec<LiveDevice> devices;
        devices.push_back(device);
        module.rust()->set_live_devices(std::move(devices));
        auto live = module.rust()->live_devices();
        require(live.size() == 1 && live[0].capabilities == mask,
                "all four capability flags round-trip through the Rust domain");
        operationRouting(module, signer, mask);
        require(keys::KeysModule::deviceXpubPaths(bits).size() == ((mask & 4) != 0 ? 4 : 3),
                "only advertised SP support adds the BIP352 path");
    }
    module.setNetwork(network);
    for (quint32 mask = 0; mask < 16; ++mask) {
        HwiDevice record;
        record.state = HwiDeviceState::Supported;
        record.capabilities = mask;
        HwiSigner signer(nullptr, record);
        auto capabilities = signer.capabilities();
        require((capabilities.ordinary == plugin::Support::Supported) == ((mask & 1) != 0) &&
                    (capabilities.taproot == plugin::Support::Supported) == ((mask & 2) != 0),
                "HWI base capabilities come from provider metadata, not readiness");
        require((capabilities.silentPayments == plugin::Support::Supported) == ((mask & 4) != 0) &&
                    (capabilities.musig2 == plugin::Support::Supported) == ((mask & 8) != 0),
                "HWI provider bits convert to public SP and MuSig2 metadata");
    }
    for (auto state : {HwiDeviceState::Locked, HwiDeviceState::Unsupported}) {
        HwiDevice record;
        record.state = state;
        record.capabilities = 0;
        HwiSigner signer(nullptr, record);
        require(signer.capabilities() == plugin::SignerCapabilities{},
                "unusable HWI records explicitly advertise no supported operation");
    }
}

template <typename Public> void bridgeValues(quint32 count) {
    for (quint32 wire = 0; wire < count; ++wire) {
        auto value = plugin::enumFromWire<Public>(wire);
        require(value.has_value(), "every supported wire enum decodes");
        auto bridge = account::toBridge(*value);
        require(bridge.has_value(), "every supported enum has a bridge conversion");
        require(account::toPublic<Public>(*bridge) == *value,
                "bridge enum round-trip preserves value");
    }
    require(!account::toBridge(static_cast<Public>(count)).has_value(),
            "bridge rejects unknown enum");
}

void publicEnums(keys::KeysModule &module) {
    bridgeValues<plugin::Network>(4);
    bridgeValues<plugin::KeyRouteKind>(5);
    bridgeValues<plugin::SpendLockKind>(5);
    bridgeValues<plugin::SpendMaturity>(3);
    bridgeValues<plugin::SpendAvailability>(4);
    bridgeValues<plugin::SpendCoinStatus>(5);
    bridgeValues<plugin::SigningStage>(8);
    bridgeValues<plugin::SigningProgress>(3);
    bridgeValues<plugin::SpendError>(7);
    auto previous = keys::KeysModule::network();
    auto networks = std::array{std::pair{plugin::Network::Bitcoin, Network::Bitcoin},
                               std::pair{plugin::Network::Testnet, Network::Testnet},
                               std::pair{plugin::Network::Signet, Network::Signet},
                               std::pair{plugin::Network::Regtest, Network::Regtest}};
    auto routes = std::array{std::pair{plugin::KeyRouteKind::Unknown, KeyRouteKind::Unknown},
                             std::pair{plugin::KeyRouteKind::Hot, KeyRouteKind::Hot},
                             std::pair{plugin::KeyRouteKind::Usb, KeyRouteKind::Usb},
                             std::pair{plugin::KeyRouteKind::Qr, KeyRouteKind::Qr},
                             std::pair{plugin::KeyRouteKind::SdCard, KeyRouteKind::SdCard}};
    for (const auto &[publicNetwork, bridgeNetwork] : networks) {
        require(account::toBridge(publicNetwork) == bridgeNetwork,
                "network names map explicitly, not by ordinal");
        module.setNetwork(bridgeNetwork);
        require(module.rust()
                    ->add_key("00112233", std::to_string(static_cast<int>(bridgeNetwork)))
                    .is_ok,
                "each network stores an isolated key record");
        auto result = module.bus()->keys(publicNetwork);
        require(result.is_ok, "public metadata lookup succeeds");
        auto entries = result.entries;
        require(entries.size() == 1 &&
                    entries.first().alias == QString::number(static_cast<int>(bridgeNetwork)),
                "public lookup selects the requested network, not a fallback");
        for (const auto &[publicRoute, bridgeRoute] : routes) {
            require(account::toBridge(publicRoute) == bridgeRoute,
                    "route names map explicitly, not by ordinal");
            KeyInfo info;
            info.fingerprint = "00112233";
            if (bridgeRoute != KeyRouteKind::Unknown) {
                info.routes.push_back(bridgeRoute);
            }
            auto entry = module.buildKeyEntry(info);
            require(entry.route == publicRoute, "every route is exported with its public meaning");
            require(entry.descriptor_route == (publicRoute == plugin::KeyRouteKind::Qr
                                                   ? plugin::KeyRouteKind::Qr
                                                   : plugin::KeyRouteKind::Unknown),
                    "descriptor route remains QR or Unknown without a hot secret");
        }
        require(module.rust()->remove_key("00112233", true).is_ok,
                "network fixture removes its key");
    }
    module.setNetwork(previous);
}

void invalidEnums(keys::KeysModule &module) {
    keys::KeysBus bus(nullptr);
    ErrorKindSpy errors;
    QObject::connect(&bus, &IKeys::error, &errors, &ErrorKindSpy::onError, qontrol::UNIQUE);
    auto invalid = static_cast<plugin::Network>(256);
    auto previous = keys::KeysModule::network();
    auto invalidResult = bus.keys(invalid);
    require(!invalidResult.is_ok && invalidResult.kind == plugin::SignError::InvalidRequest &&
                invalidResult.entries.empty(),
            "invalid network lookup is a typed failure");
    require(bus.cachedXpub(invalid, {}, {}).isEmpty(), "invalid cached lookup refuses");
    require(bus.registrationHmac(invalid, {}, {}).isEmpty(), "invalid proof lookup refuses");
    require(!bus.storeRegistrationHmac(invalid, {}, {}, {}), "invalid proof update refuses");
    bus.sign(17, invalid, {}, {}, {}, {});
    bus.signOnDevice(17, invalid, {}, {}, {});
    bus.verifyAddress(17, invalid, {}, {}, {}, {});
    bus.verifyOnDevice(17, invalid, {}, {}, {}, {});
    bus.spDescriptor(17, invalid, {});
    bus.xpub(17, invalid, {}, {});
    bus.xpubOnDevice(17, invalid, {}, {});
    bus.registerDescriptor(17, invalid, {}, {});
    bus.checkDescriptorRegistered(17, invalid, {}, {});
    require(errors.count() == 12 && errors.kind() == plugin::SignError::InvalidRequest &&
                errors.reqId() == 17,
            "all invalid-network operations emit a typed refusal before touching a module");
    require(keys::KeysModule::network() == previous,
            "invalid protocol values never select Regtest");
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "caller handle uses the real host");
    host::KeysHandle handle(host, &module, module.bus());
    ErrorKindSpy handleErrors;
    QObject::connect(&handle, &IKeys::error, &handleErrors, &ErrorKindSpy::onError,
                     qontrol::UNIQUE);
    auto reqId = handle.newRequest();
    handle.xpub(reqId, invalid, {}, {});
    require(handleErrors.count() == 1 && handleErrors.reqId() == reqId &&
                handleErrors.kind() == plugin::SignError::InvalidRequest,
            "caller handle refuses an invalid network before forwarding");
    auto handleResult = handle.keys(invalid);
    require(!handleResult.is_ok && handleResult.kind == plugin::SignError::InvalidRequest &&
                handleResult.entries.empty() && handleErrors.count() == 1,
            "synchronous caller lookup returns a typed error, not an empty successful list");
    KeysBoundaryProbe probe;
    QObject::connect(module.bus(), &IKeys::coordinationError, &probe,
                     &KeysBoundaryProbe::onCoordinationError, qontrol::UNIQUE);
    plugin::SigningRequest valid;
    valid.inputs.append(plugin::SelectedInput{});
    for (auto field : {0, 1, 2, 3, 4, 5, 6}) {
        auto request = valid;
        auto &variant = request.inputs.first().variant;
        switch (field) {
        case 0:
            request.network = invalid;
            break;
        case 1:
            request.stage = static_cast<plugin::SigningStage>(256);
            break;
        case 2:
            request.inputs.first().network = invalid;
            break;
        case 3:
            variant.absoluteLock.kind = static_cast<plugin::SpendLockKind>(256);
            break;
        case 4:
            variant.relativeLock.kind = static_cast<plugin::SpendLockKind>(256);
            break;
        case 5:
            variant.maturity = static_cast<plugin::SpendMaturity>(256);
            break;
        case 6:
            variant.availability = static_cast<plugin::SpendAvailability>(256);
            break;
        }
        require(!account::validRequest(request), "invalid nested enum fails request validation");
        module.bus()->coordinate(91, request);
        require(probe.answers == field + 1 && probe.request == 91 &&
                    probe.kind == plugin::SpendError::InvalidRequest,
                "invalid coordination request emits one typed error without allocating an attempt");
        TransactionJob job(91, TransactionJob::Operation::Finalize, request);
        job.start();
        job.wait();
        require(!job.result().is_ok && job.result().kind == SpendError::InvalidRequest,
                "transaction service rejects invalid public enum before Rust");
    }
}
} // namespace

void keysContractSmoke(const QString &screenshots) {
    Theme::get()->apply();
    auto module = std::make_unique<keys::KeysModule>();
    publicEnums(*module);
    invalidEnums(*module);
    signerCapabilities(*module);
    auto *keysModule = module.get();
    AppController::get()->registerBuiltinPlugin(std::move(module));
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "real host is available");
    keysModule->startInstance(QStringLiteral("keys"));
    require(host->instanceOwner(QStringLiteral("keys")) == nullptr,
            "failed publication before shell creation unregisters its instance");
    MainWindow window;
    AppController::get()->start(&window);
    window.resize(1440, 1000);
    window.show();
    auto *tabs = window.findChild<catalog::Tabs *>();
    auto *bar = window.findChild<QTabBar *>(QString(), Qt::FindChildrenRecursively);
    for (auto *candidate : window.findChildren<QTabBar *>()) {
        if (candidate->property("class") == QStringLiteral("module-tabs")) {
            bar = candidate;
        }
    }
    require(tabs != nullptr && bar != nullptr, "real shell tab controls exist");
    auto *launcher = tabs->currentWidget();
    QApplication::processEvents();
    require(bar->count() == 2 && bar->tabText(0) == QStringLiteral("Keys") &&
                bar->tabText(1).isEmpty(),
            "Keys precedes the Settings gear");
    require(tabs->currentWidget() == launcher, "publication preserves launcher selection");
    auto *settings = qobject_cast<QWidget *>(bar->tabData(1).value<QObject *>());
    require(settings != nullptr && !bar->findChildren<QToolButton *>().empty(),
            "Settings page and gear survive");
    clickTab(bar, 1);
    require(tabs->currentWidget() == settings, "Settings gear click opens its existing page");
    require(host->instanceOwner(QStringLiteral("keys")) == keysModule,
            "Keys registers an owned host instance");
    QPointer<QWidget> page = qobject_cast<QWidget *>(bar->tabData(0).value<QObject *>());
    require(page != nullptr && !tabs->isTabVisible(tabs->indexOf(page)),
            "module page is hidden in account strip");
    QEvent otherPageTitle(QEvent::LanguageChange);
    QApplication::sendEvent(page, &otherPageTitle);
    selectedPage(tabs, bar, settings, true);
    clickTab(bar, 0);
    selectedPage(tabs, bar, page, true);
    require(tabs->currentWidget() == page, "Keys module tab click opens the published page");
    bar->setTabText(0, QStringLiteral("stale title"));
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(page, &languageChange);
    require(bar->tabText(0) == QStringLiteral("Keys"),
            "page language event refreshes its host title");
    selectedPage(tabs, bar, page, true);
    keysModule->startInstance(QStringLiteral("keys"));
    require(bar->count() == 2, "duplicate start does not duplicate the page");
    AppController::get()->showKeysTab();
    require(tabs->currentWidget() == page, "shortcut uses host instance page routing");
    QApplication::processEvents();
    settingsGear(bar);
    if (!screenshots.isEmpty()) {
        require(window.grab().save(QDir(screenshots).filePath(QStringLiteral("keys-after.png"))),
                "full Keys capture saves");
    }
    window.onTabCloseRequested(tabs->indexOf(page));
    require(host->instanceOwner(QStringLiteral("keys")) == keysModule,
            "module page ignores account close requests");
    MockInstance account(QStringLiteral("placement-account"));
    auto *accountPage = new QWidget;
    require(host->openTab(accountPage, &account) == 0,
            "unregistered owners cannot publish long-lived UI");
    host->registerInstance(account.id(), &account, keysModule);
    auto accountTab = host->openTab(accountPage, &account);
    host->setTabTitle(accountTab, QStringLiteral("Account"));
    require(tabs->currentWidget() == accountPage && tabs->isTabVisible(tabs->indexOf(accountPage)),
            "normal account page keeps its strip and focus");
    selectedPage(tabs, bar, accountPage, false);
    require(host->openTab(accountPage, &account) == accountTab,
            "reopening the same owner's content keeps the existing tab");
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(tabs->indexOf(accountPage) >= 0, "duplicate publication does not delete the live page");
    auto *rejected = new QWidget;
    require(host->openTab(rejected, &account, static_cast<Host::PagePlacement>(255)) == 0,
            "invalid placement is rejected before duplicate-owner handling");
    delete rejected;
    host->removeInstance(QStringLiteral("keys"), keysModule);
    selectedPage(tabs, bar, accountPage, false);
    require(page.isNull(), "removal destroys Keys page before module lifetime ends");
    require(bar->count() == 1 && bar->tabData(0).value<QObject *>() == settings &&
                tabs->indexOf(accountPage) >= 0 && tabs->currentWidget() == accountPage,
            "removal preserves Settings and the active account");
    host->showInstanceTab(QStringLiteral("keys"));
    require(tabs->currentWidget() == accountPage, "removed page shortcut cannot use stale handles");
    QApplication::processEvents();
    settingsGear(bar);
    if (!screenshots.isEmpty()) {
        require(window.grab().save(QDir(screenshots).filePath(QStringLiteral("keys-removed.png"))),
                "removed-page full-window capture saves");
    }
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    keysModule->startInstance(QStringLiteral("keys"));
    selectedPage(tabs, bar, accountPage, false);
    require(bar->count() == 2 && bar->tabData(1).value<QObject *>() == settings,
            "recreation publishes a fresh page before Settings");
    AppController::get()->showKeysTab();
    selectedPage(tabs, bar, tabs->currentWidget(), true);
    require(tabs->currentWidget() != accountPage && tabs->indexOf(accountPage) >= 0,
            "recreated page shortcut works without losing accounts");
    QApplication::processEvents();
    if (!screenshots.isEmpty()) {
        require(
            window.grab().save(QDir(screenshots).filePath(QStringLiteral("keys-recreated.png"))),
            "recreated full-window capture saves");
    }
    host->removeInstance(QStringLiteral("keys"), keysModule);
    host->closeTab(accountTab);
    host->removeInstance(account.id(), keysModule);
    MockInstance generic(QStringLiteral("generic-module-page"));
    host->registerInstance(generic.id(), &generic, keysModule);
    auto *genericPage = new QWidget;
    auto genericTab = host->openTab(genericPage, &generic, Host::PagePlacement::Module);
    host->setTabTitle(genericTab, QStringLiteral("External page"));
    require(bar->count() == 2 && bar->tabText(0) == QStringLiteral("External page") &&
                bar->tabData(1).value<QObject *>() == settings,
            "the same placement works for non-Keys module pages");
    host->setTabTitle(genericTab, QStringLiteral("Renamed page"));
    require(bar->tabText(0) == QStringLiteral("Renamed page"), "generic module page titles update");
    window.showTab(settings);
    selectedPage(tabs, bar, settings, true);
    host->setTabTitle(genericTab, QStringLiteral("Renamed while Settings is active"));
    selectedPage(tabs, bar, settings, true);
    host->closeTab(genericTab, false);
    selectedPage(tabs, bar, settings, true);
    require(bar->count() == 1 && genericPage->parentWidget() == nullptr,
            "detaching module content removes strip references without deleting it");
    delete genericPage;
    genericPage = new QWidget;
    genericTab = host->openTab(genericPage, &generic, Host::PagePlacement::Module);
    host->setTabTitle(genericTab, QStringLiteral("Recreated while Settings is active"));
    selectedPage(tabs, bar, settings, true);
    window.showTab(genericPage);
    selectedPage(tabs, bar, genericPage, true);
    host->setTabTitle(genericTab, QStringLiteral("Active page renamed"));
    selectedPage(tabs, bar, genericPage, true);
    window.showTab(settings);
    host->closeTab(genericTab);
    host->removeInstance(generic.id(), keysModule);
    selectedPage(tabs, bar, settings, true);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    keysMetadataSmoke(*keysModule);
}

} // namespace smoke
