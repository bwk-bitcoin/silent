#include "MainWindow.h"
#include "AppController.h"
#include "catalog/containers/Tabs.h"
#include "host/Host.h"
#include "i18n/Tr.h"
#include "interfaces/instance.h"
#include "theme/Icon.h"
#include "theme/Palette.h"
#include "views/DashboardLayout.h"
#include "views/MenuTab.h"
#include "views/modals/AppSettings.h"
#include <QPointer>
#include <QStyle>
#include <QToolButton>
#include <QVariant>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <qlogging.h>
#include <qtabbar.h>
#include <qtabwidget.h>
#include <qwidget.h>

namespace {
class ModuleTabBar final : public QTabBar {
public:
    ModuleTabBar(QTabWidget *tabs, QWidget *menu) : m_tabs(tabs), m_menu(menu) {
    }

    void setGear(int index) {
        auto *button = new QToolButton(this);
        button->setIcon(icon::settings());
        button->setIconSize(QSize(metric::TAB_ICON_SIZE, metric::TAB_ICON_SIZE));
        button->setFixedSize(resolve(Spacing::M), resolve(Spacing::M));
        button->setAutoRaise(true);
        button->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_gear = button;
        setTabButton(index, QTabBar::RightSide, button);
        centerGear();
    }

protected:
    [[nodiscard]] auto tabSizeHint(int index) const -> QSize override {
        QSize size = QTabBar::tabSizeHint(index);
        if (index == count() - 1) {
            size.setWidth(m_tabs->tabBar()->tabRect(m_tabs->indexOf(m_menu)).width());
        }
        return size;
    }

    void tabLayoutChange() override {
        QTabBar::tabLayoutChange();
        centerGear();
    }

    void paintEvent(QPaintEvent *event) override {
        centerGear();
        QTabBar::paintEvent(event);
    }

    void centerGear() {
        if (m_gear == nullptr || count() == 0 ||
            tabButton(count() - 1, QTabBar::RightSide) != m_gear) {
            return;
        }
        QRect centered = QStyle::alignedRect(layoutDirection(), Qt::AlignCenter, m_gear->size(),
                                             tabRect(count() - 1));
        if (m_gear->pos() != centered.topLeft()) {
            m_gear->move(centered.topLeft());
        }
    }

private:
    QTabWidget *m_tabs = nullptr;
    QWidget *m_menu = nullptr;
    QPointer<QToolButton> m_gear;
};
} // namespace

MainWindow::MainWindow(QWidget *parent) : Window(parent) {
    resize(1024, 768);
    // Never shrink below the sidebar plus the dashboard's minimum content width
    // (with its horizontal padding), so screens never clip horizontally.
    setMinimumWidth(metric::SIDEBAR_WIDTH + DashboardLayout::MIN_CONTENT_WIDTH +
                    2 * resolve(Spacing::S));
    initWindow();
    retranslateUi();
}

auto MainWindow::initWindow() -> void {
    if (m_init)
        return;

    m_tab = new catalog::Tabs(this);
    m_menu_tab = new MenuTab(this);
    m_keys_tab = AppController::get()->keysTab();
    m_settings_tab = new modal::AppSettings;

    m_tab->setTabsClosable(true);
    m_tab->setMovable(true);

    // A tab bar always has a current tab: while an account or the launcher shows, the
    // bar is idle and its current tab is drawn as not selected.
    m_module_bar = new ModuleTabBar(m_tab, m_menu_tab);
    m_module_bar->setProperty("class", "module-tabs");
    m_module_bar->setDrawBase(false);
    m_module_bar->setExpanding(false);
    // As wide as its tabs: no room kept for scroll buttons, at the end of the strip.
    m_module_bar->setUsesScrollButtons(false);
    m_module_bar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto *corner = (new qontrol::Row)
                       ->margins(resolve(Spacing::XS), 0, resolve(Padding::XS), 0)
                       ->pushAligned(m_module_bar, Qt::AlignBottom);
    m_tab->setCornerWidget(corner, Qt::TopRightCorner);
    m_tab->setUsesScrollButtons(true);

    updateTabs();
    // The app opens on the launcher; an account opened afterwards takes its place.
    m_tab->setCurrentWidget(m_menu_tab);

    setCentralWidget(m_tab);

    connect(m_tab, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested,
            qontrol::UNIQUE);
    connect(m_tab, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged,
            qontrol::UNIQUE);
    // Clicked, not current changed: the idle bar's current tab is clicked to open it.
    connect(m_module_bar, &QTabBar::tabBarClicked, this, &MainWindow::onModuleTabClicked,
            qontrol::UNIQUE);

    m_init = true;
}

auto MainWindow::updateTabs() -> void {
    // Removing every tab moves the current one to the first tab added back: keep it.
    QWidget *current = m_tab->currentWidget();
    while (m_tab->count() > 0) {
        m_tab->removeTab(0);
    }

    for (const auto &tab : m_tabs) {
        int index = m_tab->addTab(tab.second, tab.first);
        auto *btn = m_tab->tabBar()->tabButton(index, QTabBar::RightSide);
        if (btn != nullptr) {
            btn->setToolTip("");
        }
    }

    // Menu tab has no close button
    int menuIndex = m_tab->addTab(m_menu_tab, TR("main-menu-tab"));
    m_tab->tabBar()->setTabButton(menuIndex, QTabBar::RightSide, nullptr);

    // A module's page sits in the tab widget, its tab hidden there: the module bar
    // shows it on the right.
    for (const auto &[page, title] : modulePages()) {
        int index = m_tab->addTab(page, title);
        m_tab->tabBar()->setTabButton(index, QTabBar::RightSide, nullptr);
        m_tab->tabBar()->setTabVisible(index, false);
    }
    updateModuleTabs();

    if (current != nullptr && m_tab->indexOf(current) >= 0) {
        m_tab->setCurrentWidget(current);
    }
    onCurrentTabChanged();
}

auto MainWindow::modulePages() const -> QList<QPair<QWidget *, QString>> {
    QList<QPair<QWidget *, QString>> pages;
    if (m_keys_tab != nullptr) {
        pages.append({m_keys_tab, TR("main-keys-tab")});
    }
    // The global settings, right of Keys (ws-plaude/silent#46).
    pages.append({m_settings_tab, TR("main-settings-tab")});
    return pages;
}

void MainWindow::updateModuleTabs() {
    while (m_module_bar->count() > 0) {
        m_module_bar->removeTab(0);
    }
    for (const auto &[page, title] : modulePages()) {
        int index = m_module_bar->addTab(page == m_settings_tab ? QString() : title);
        if (page == m_settings_tab) {
            static_cast<ModuleTabBar *>(m_module_bar)->setGear(index);
            m_module_bar->setTabToolTip(index, title);
        }
        m_module_bar->setTabData(index, QVariant::fromValue(static_cast<QObject *>(page)));
    }
}

void MainWindow::showKeysTab() {
    if (m_keys_tab != nullptr) {
        m_tab->setCurrentWidget(m_keys_tab);
    }
}

auto MainWindow::tabExists(QWidget *content) const -> bool {
    return std::ranges::any_of(m_tabs,
                               [content](const auto &tab) { return tab.second == content; });
}

auto MainWindow::addTab(QWidget *content, const QString &title) -> int {
    if (content == nullptr || tabExists(content)) {
        return -1;
    }
    m_tabs.append(QPair<QString, QWidget *>(title, content));
    updateTabs();
    // The account just opened is the one to show.
    m_tab->setCurrentWidget(content);
    return m_tab->indexOf(content);
}

auto MainWindow::removeTab(QWidget *content, bool delete_content) -> void {
    if (content == nullptr) {
        return;
    }

    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs.at(i).second == content) {
            int tabIndex = m_tab->indexOf(content);
            if (tabIndex >= 0) {
                m_tab->removeTab(tabIndex);
            }
            m_tabs.removeAt(i);
            if (delete_content) {
                content->deleteLater();
            } else {
                content->setParent(nullptr);
            }
            return;
        }
    }
}

auto MainWindow::setTabTitle(QWidget *content, const QString &title) -> void {
    if (content == nullptr) {
        return;
    }
    for (auto &tab : m_tabs) {
        if (tab.second == content) {
            tab.first = title;
            int tabIndex = m_tab->indexOf(content);
            if (tabIndex >= 0) {
                m_tab->setTabText(tabIndex, title);
            }
            return;
        }
    }
}

auto MainWindow::changeEvent(QEvent *event) -> void {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        updateTabs();
    }
    Window::changeEvent(event);
}

auto MainWindow::retranslateUi() -> void {
    setWindowTitle(TR("main-window-title"));
}

auto MainWindow::closeEvent(QCloseEvent *event) -> void {
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    if (host != nullptr) {
        auto activeInstances = host->liveInstances();
        for (auto *instance : activeInstances) {
            if (instance != nullptr) {
                instance->stop();
            }
        }
    }
    event->accept();
    Window::closeEvent(event);
}

auto MainWindow::onTabCloseRequested(int index) -> void {
    auto *hosted = m_tab->widget(index);
    if (hosted != nullptr && hosted != m_menu_tab && hosted != m_keys_tab &&
        hosted != m_settings_tab) {
        emit hostedTabCloseRequested(hosted);
    }
}

void MainWindow::onCurrentTabChanged() {
    // Select the current page's module tab, or idle the bar for any other page.
    QWidget *current = m_tab->currentWidget();
    int moduleIndex = -1;
    for (int i = 0; i < m_module_bar->count(); ++i) {
        if (m_module_bar->tabData(i).value<QObject *>() == current) {
            moduleIndex = i;
        }
    }
    if (moduleIndex >= 0) {
        m_module_bar->setCurrentIndex(moduleIndex);
    }
    bool idle = moduleIndex < 0;
    if (m_module_bar->property("idle").toBool() != idle) {
        m_module_bar->setProperty("idle", idle);
        m_module_bar->style()->unpolish(m_module_bar);
        m_module_bar->style()->polish(m_module_bar);
    }
}

void MainWindow::onModuleTabClicked(int index) {
    auto *page = qobject_cast<QWidget *>(m_module_bar->tabData(index).value<QObject *>());
    if (page != nullptr) {
        m_tab->setCurrentWidget(page);
    }
}

MainWindow::~MainWindow() = default;
