#include "AppController.h"
#include "MainWindow.h"
#include "i18n/I18nManager.h"
#include "interfaces/host.h"
#include "interfaces/types.h"
#include "metatypes.h"
#include "modules/hwi/HwiPlugin.h"
#include "modules/keys/KeysModule.h"
#include "modules/sp/SpPlugin.h"
#include "qr/QrTypes.h"
#include "resources/font/noto_sans.h"
#include "resources/font/noto_sans_bold.h"
#include "resources/font/noto_sans_mono.h"
#include "resources/font/noto_sans_mono_bold.h"
#include "theme/Theme.h"
#include <QApplication>
#include <QFontDatabase>
#include <QStyleFactory>
#include <QtPlugin>
#include <common.h>
#include <memory>

#ifdef SILENT_STATIC_PLUGINS
Q_IMPORT_PLUGIN(SpPlugin)
Q_IMPORT_PLUGIN(HwiPlugin)
#endif

auto main(int argc, char *argv[]) -> int {
#ifdef Q_OS_LINUX
    // On KDE the file dialog runs in our process and takes the app's theme: through the
    // portal it runs in its own and follows the OS theme. Read only by KDE's platform
    // theme, before the application exists.
    if (!qEnvironmentVariableIsSet("PLASMA_INTEGRATION_USE_PORTAL")) {
        qputenv("PLASMA_INTEGRATION_USE_PORTAL", "1");
    }
#endif
    QApplication app(argc, argv);
#ifdef Q_OS_LINUX
    QApplication::setStyle(QStyleFactory::create("Fusion"));
#endif

    // Embed default font
    QFontDatabase::addApplicationFontFromData(QByteArray(
        reinterpret_cast<const char *>(embedded_font::NOTO_SANS), embedded_font::NOTO_SANS_SIZE));
    QFontDatabase::addApplicationFontFromData(
        QByteArray(reinterpret_cast<const char *>(embedded_font::NOTO_SANS_MONO),
                   embedded_font::NOTO_SANS_MONO_SIZE));
    // font::DEMIBOLD and Bold resolve to the real Bold faces, as in the design, instead
    // of a bold synthesized from the regular face where Noto is not installed.
    QFontDatabase::addApplicationFontFromData(
        QByteArray(reinterpret_cast<const char *>(embedded_font::NOTO_SANS_BOLD),
                   embedded_font::NOTO_SANS_BOLD_SIZE));
    QFontDatabase::addApplicationFontFromData(
        QByteArray(reinterpret_cast<const char *>(embedded_font::NOTO_SANS_MONO_BOLD),
                   embedded_font::NOTO_SANS_MONO_BOLD_SIZE));

    // Register CXX types for cross-thread signal-slot connections
    qRegisterMetaType<BackendInfo>();
    qRegisterMetaType<ConnectionResult>();
    qRegisterMetaType<TxResult>();
    qRegisterMetaType<Notification>();
    qRegisterMetaType<HwiEvent>();
    qRegisterMetaType<HwiDevice>();
    qRegisterMetaType<qr::QrRender>();
    qRegisterMetaType<qr::QrScan>();
    qRegisterMetaType<MnemonicResult>();
    qRegisterMetaType<KeyInfoResult>();
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
    controller->registerBuiltinPlugin(std::make_unique<keys::KeysModule>());

    // Create and show main window
    auto *window = new MainWindow();
    controller->start(window);

    // Initialize state (load accounts)
    controller->initState();

    // Show window
    window->show();

    return QApplication::exec();
}
