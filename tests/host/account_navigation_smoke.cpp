#include "account_navigation_smoke.h"

#include "AppController.h"
#include "MainWindow.h"
#include "PluginStopProbe.h"
#include "account/AccountModule.h"
#include "account/AccountShell.h"
#include "host/PluginRegistry.h"
#include "interfaces/host.h"
#include "interfaces/module.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/MusigAccountWidget.h"
#include "modules/musig/MusigStatusBar.h"
#include "modules/sp/AccountController.h"
#include "modules/sp/AccountWidget.h"
#include "modules/sp/StatusBar.h"
#include "theme/Palette.h"
#include "theme/Theme.h"
#include "views/MenuTab.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QWidget>
#include <Qontrol>
#include <common.h>
#include <functional>
#include <type_traits>

#ifdef SILENT_CUSTOM_WIDGET_FIXTURE
#include "PluginBundleFixture.h"
#include <QFile>
#endif

namespace smoke {
namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("account navigation: %s", message);
    }
}

void waitFor(const std::function<bool()> &ready) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < 20000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "operation timed out");
    QApplication::processEvents();
}

void click(QAbstractButton *button) {
    auto position = QPointF(button->rect().center());
    auto global = QPointF(button->mapToGlobal(position.toPoint()));
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(button, &press);
    QApplication::sendEvent(button, &release);
    QApplication::processEvents();
}

auto menuButton(QButtonGroup &group, const QString &label) -> QAbstractButton * {
    for (auto *button : group.buttons()) {
        if (button->text() == QStringLiteral("  ") + label) {
            return button;
        }
    }
    qFatal("account navigation: missing button %s", qPrintable(label));
}

class StopSpy : public QObject {
public:
    void onStopped() {
        ++count;
    }

    int count = 0;
};

struct Entry {
    QString name;
    QString label;
};

template <typename Widget, typename Status>
void navigation(MainWindow &window, const QString &name, const QList<Entry> &entries) {
    auto *widget = new Widget(name);
    auto *controller = widget->controller();
    // The panels' services come from the account module, for the accounts it watches.
    qobject_cast<account::AccountModule *>(controller->module())
        ->watchAccount(name, controller, controller->services());
    controller->loadPanel(QStringLiteral("settings"));
    window.addTab(widget, name);
    waitFor([controller] {
        return !controller->accountLoading() && !controller->endpointOperationsBusy();
    });
    auto groups = widget->template findChildren<QButtonGroup *>();
    require(groups.size() == 1, "one shared sidebar button group");
    auto *group = groups.first();
    require(group->buttons().size() == entries.size(), "no module placeholder entries");
    require(widget->template findChild<Status *>() != nullptr, "module status retained");
    auto check = [&](const Entry &selected) {
        require(group->checkedButton() == menuButton(*group, selected.label),
                "exact translated label selected");
        int checked = 0;
        for (auto *button : group->buttons()) {
            checked += button->isChecked() ? 1 : 0;
        }
        require(checked == 1, "exactly one checked button");
        for (const auto &entry : entries) {
            auto screen = controller->screen(entry.name);
            require(screen.has_value() &&
                        screen.value()->isVisible() == (entry.name == selected.name),
                    "only selected panel visible");
        }
    };
    check({.name = "settings", .label = "Settings"});
    for (const auto &entry : entries) {
        click(menuButton(*group, entry.label));
        check(entry);
    }
    for (const auto &entry : entries) {
        controller->loadPanel(entry.name);
        check(entry);
        controller->loadPanel(entry.name);
        check(entry);
    }
    controller->loadPanel(QStringLiteral("unknown"));
    check(entries.last());
    controller->loadPanels();
    check(entries.last());
    QList<QPointer<qontrol::Screen>> screens;
    for (const auto &entry : entries) {
        screens.append(controller->screen(entry.name).value());
    }
    QPointer<QObject> backend = controller;
    StopSpy stopped;
    QObject::connect(controller, &std::remove_pointer_t<decltype(controller)>::stopped, &stopped,
                     &StopSpy::onStopped, qontrol::UNIQUE);
    controller->stop();
    waitFor([&stopped] { return stopped.count == 1; });
    window.removeTab(widget);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(backend == nullptr, "backend destroyed on close");
    for (const auto &screen : screens) {
        require(screen == nullptr, "all panel screens destroyed on close");
    }
}

class EmptyScreen : public qontrol::Screen {
protected:
    void init() override {
    }

    void doConnect() override {
    }

    void view() override {
    }
};

void unavailablePanels() {
    QWidget parent;
    account::AccountShell shell(account::accountMenuEntries(), new QWidget, &parent);
    shell.widget()->show();
    parent.show();
    shell.insertPanel(new qontrol::Panel(new EmptyScreen, "settings"));
    shell.showPanel(QStringLiteral("settings"));
    auto *group = shell.findChild<QButtonGroup *>();
    click(menuButton(*group, QStringLiteral("History")));
    require(group->checkedButton()->text() == QStringLiteral("  Settings"),
            "unavailable clicked panel retains displayed selection");
    shell.showPanel(QStringLiteral("receive"));
    shell.insertPanel(new qontrol::Panel(new EmptyScreen, "unknown"));
    shell.showPanel(QStringLiteral("unknown"));
    require(group->checkedButton()->text() == QStringLiteral("  Settings") &&
                shell.screen(QStringLiteral("settings")).value()->isVisible() &&
                !shell.screen(QStringLiteral("unknown")).value()->isVisible(),
            "missing and unknown panels leave current view unchanged");
    click(menuButton(*group, QStringLiteral("Coins")));
}

// The color of the sidebar's padding, left of the menu buttons.
auto sidebarColor(QWidget *sidebar) -> QColor {
    QCoreApplication::processEvents();
    return sidebar->grab().toImage().pixelColor(1, sidebar->height() / 2);
}

// A theme switch repaints an account page's sidebar in the new theme, not only after a
// restart (#84).
void sidebarFollowsTheme() {
    auto *theme = Theme::get();
    auto initial = theme->mode();
    QWidget parent;
    account::AccountShell shell(account::accountMenuEntries(), new QWidget, &parent);
    parent.resize(1000, 700);
    parent.show();
    QWidget *sidebar = nullptr;
    for (auto *widget : shell.widget()->findChildren<QWidget *>()) {
        if (widget->minimumWidth() == metric::SIDEBAR_WIDTH &&
            widget->maximumWidth() == metric::SIDEBAR_WIDTH) {
            sidebar = widget;
        }
    }
    require(sidebar != nullptr, "the account page has a sidebar");
    theme->setMode(ThemeMode::Dark);
    require(sidebarColor(sidebar) == Palette::dark().bgSecondary,
            "the sidebar takes the dark theme on a switch");
    theme->setMode(ThemeMode::Light);
    require(sidebarColor(sidebar) == Palette::light().bgSecondary,
            "the sidebar takes the light theme on a switch back");
    theme->setMode(initial);
}

#ifdef SILENT_CUSTOM_WIDGET_FIXTURE
auto fixtureModule(PluginRegistry &registry, const QString &id) -> IModule * {
    for (auto *plugin : registry.plugins()) {
        for (auto *module : plugin->modules()) {
            if (module->meta().value(QStringLiteral("id")) == id) {
                return module;
            }
        }
    }
    return nullptr;
}

auto launcherButton(MenuTab &launcher, const QString &id) -> QPushButton * {
    for (auto *button : launcher.findChildren<QPushButton *>()) {
        if (button->property("instanceId").toString() == id && button->isVisibleTo(&launcher)) {
            return button;
        }
    }
    return nullptr;
}

void openRemainingAccount(MainWindow &window, MenuTab &launcher, PluginRegistry &registry) {
    window.showTab(&launcher);
    launcher.onRefreshLauncherItems();
    QApplication::processEvents();
    auto *button = launcherButton(launcher, QStringLiteral("boundary-sp"));
    require(button != nullptr && button->isEnabled(), "remaining launcher account is available");
    click(button);
    auto *widget = window.findChild<AccountWidget *>();
    require(widget != nullptr, "remaining launcher opens real SP account after plugin unload");
    auto *controller = widget->controller();
    waitFor([controller] {
        return !controller->accountLoading() && !controller->endpointOperationsBusy();
    });
    controller->loadPanel(QStringLiteral("receive"));
    auto *group = widget->findChild<QButtonGroup *>();
    require(group != nullptr && group->checkedButton()->text() == QStringLiteral("  Receive") &&
                controller->screen("receive").value()->isVisible(),
            "remaining account navigation still operates after unload");
    StopSpy stopped;
    QObject::connect(controller, &AccountController::stopped, &stopped, &StopSpy::onStopped,
                     qontrol::UNIQUE);
    Host::get()->removeInstance(QStringLiteral("boundary-sp"),
                                fixtureModule(registry, QStringLiteral("sp.module")));
    waitFor([&stopped] { return stopped.count == 1; });
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(window.findChild<AccountWidget *>() == nullptr, "remaining account closes safely");
}

void customWidget(MainWindow &window) {
    QFile library(QString::fromUtf8(SILENT_CUSTOM_WIDGET_FIXTURE));
    require(library.open(QIODevice::ReadOnly), "read external widget fixture");
    require(installBundle(QStringLiteral("custom.widget.fixture"),
                          QStringLiteral("Custom widget fixture"), library.readAll())
                .is_ok,
            "install SDK-only fixture");
    auto *registry = AppController::get()->pluginRegistry();
    rescanPluginsAndWait(registry);
    QPointer<MenuTab> launcher = window.findChild<MenuTab *>();
    require(launcher != nullptr, "real launcher remains alive throughout fixture unload");
    for (int visit = 0; visit < 2; ++visit) {
        require(enablePluginAndWait(registry, QStringLiteral("custom.widget.fixture")),
                "load or re-enable external widget fixture");
        QPointer<IModule> module = fixtureModule(*registry, QStringLiteral("custom.widget.module"));
        require(module != nullptr, "external module is live");
        window.showTab(launcher);
        launcher->onRefreshLauncherItems();
        QApplication::processEvents();
        auto *button = launcherButton(*launcher, QStringLiteral("custom-widget-instance"));
        require(button != nullptr && button->isEnabled(), "launcher lists reloaded fixture");
        click(button);
        auto widgets = window.findChildren<QPushButton *>(QStringLiteral("custom-plugin-widget"));
        require(widgets.size() == 1, "exactly one recreated custom page with no dangling page");
        auto *widget = widgets.first();
        require(widget->isVisible() && widget->text() == QStringLiteral("Independent plugin UI") &&
                    !widget->isChecked(),
                "fresh custom widget published without shell inheritance or panel contract");
        click(widget);
        require(widget->isChecked(), "custom widget handles mouse input");
        QPointer<QWidget> view = widget;
        require(stopPlugin(registry, QStringLiteral("custom.widget.fixture")).isEmpty(),
                "disable and unload fixture while its widget and launcher are alive");
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(view == nullptr && module == nullptr &&
                    !registry->isPluginEnabled(QStringLiteral("custom.widget.fixture")),
                "live disable destroys page and module");
        require(window.isVisible() && launcher != nullptr,
                "main window and original launcher survive plugin unload");
        window.showTab(launcher);
        launcher->onRefreshLauncherItems();
        require(launcherButton(*launcher, QStringLiteral("custom-widget-instance")) == nullptr &&
                    window.findChild<QPushButton *>(QStringLiteral("custom-plugin-widget")) ==
                        nullptr,
                "unloaded fixture has no stale launcher entry or page");
        openRemainingAccount(window, *launcher, *registry);
    }
}
#endif

} // namespace

void accountNavigationSmoke() {
    {
        MainWindow window;
        AppController::get()->start(&window);
        window.resize(1440, 1000);
        window.show();
        QList<Entry> sp{{.name = "history", .label = "History"},
                        {.name = "coins", .label = "Coins"},
                        {.name = "send", .label = "Send"},
                        {.name = "receive", .label = "Receive"},
                        {.name = "settings", .label = "Settings"}};
        auto musig = sp;
        musig.insert(musig.size() - 1, {.name = "psbt", .label = "PSBT"});
        for (int repeat = 0; repeat < 2; ++repeat) {
            navigation<AccountWidget, StatusBar>(window, QStringLiteral("boundary-sp"), sp);
            navigation<MusigAccountWidget, MusigStatusBar>(
                window, QStringLiteral("boundary-policy"), musig);
        }
        unavailablePanels();
#ifdef SILENT_CUSTOM_WIDGET_FIXTURE
        customWidget(window);
        navigation<AccountWidget, StatusBar>(window, QStringLiteral("boundary-sp"), sp);
        navigation<MusigAccountWidget, MusigStatusBar>(window, QStringLiteral("boundary-policy"),
                                                       musig);
#endif
    }
    // After the main window: a theme switch restyles every widget of the app.
    sidebarFollowsTheme();
    AppController::get()->start(nullptr);
}

} // namespace smoke
