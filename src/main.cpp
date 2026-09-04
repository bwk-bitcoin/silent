#include "AppController.h"
#include "MainWindow.h"
#include "i18n/I18nManager.h"
#include "metatypes.h"
#include "modules/hwi/HwiPlugin.h"
#include "modules/sp/SpPlugin.h"
#include "interfaces/types.h"
#include "interfaces/host.h"
#include "resources/font/noto_sans.h"
#include "resources/font/noto_sans_mono.h"
#include "theme/Theme.h"
#include <common.h>
#include <QApplication>
#include <QFontDatabase>
#include <QStyleFactory>
#include <QtPlugin>
#include <memory>

#ifdef SILENT_STATIC_PLUGINS
Q_IMPORT_PLUGIN(SpPlugin)
Q_IMPORT_PLUGIN(HwiPlugin)
#endif

auto main(int argc, char *argv[]) -> int {
    QApplication app(argc, argv);
#ifdef Q_OS_LINUX
    QApplication::setStyle(QStyleFactory::create("Fusion"));
#endif

    // Embed default font
    QFontDatabase::addApplicationFontFromData(QByteArray(
        reinterpret_cast<const char *>(embedded_font::NOTO_SANS), embedded_font::NOTO_SANS_SIZE));
    QFontDatabase::addApplicationFontFromData(QByteArray(
        reinterpret_cast<const char *>(embedded_font::NOTO_SANS_MONO), embedded_font::NOTO_SANS_MONO_SIZE));

    // Register CXX types for cross-thread signal-slot connections
    qRegisterMetaType<BackendInfo>();
    qRegisterMetaType<ConnectionResult>();
    qRegisterMetaType<TxResult>();
    qRegisterMetaType<Notification>();
    qRegisterMetaType<HwiEvent>();
    qRegisterMetaType<HwiDevice>();
    qRegisterMetaType<std::optional<ReqId>>();

    // Initialize Rust logging (Debug level for verbose output)
    init_logging(LogLevel::Error);

    // Initialize and apply default theme; persisted provider themes are applied by AppController.
    Theme::init();
    Theme::get()->apply();

    // Initialize i18n before creating UI widgets
    i18n::I18nManager::get()->init(&app);

    // Initialize application controller
    AppController::init();
    auto *controller = AppController::get();

    // Initialize host singleton and register built-in plugins before controller state setup.
    Host::get();
    controller->registerBuiltinPlugin(std::make_unique<SpPlugin>());
    controller->registerBuiltinPlugin(std::make_unique<HwiPlugin>());

    // Create and show main window
    auto *window = new MainWindow();
    controller->start(window);

    // Initialize state (load accounts)
    controller->initState();

    // Show window
    window->show();

    return QApplication::exec();
}
