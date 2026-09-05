#include "MainWindow.h"
#include "AppController.h"
#include "host/Host.h"
#include "interfaces/instance.h"
#include "i18n/Tr.h"
#include "views/DashboardLayout.h"
#include "views/MenuTab.h"
#include "catalog/Button.h"
#include "theme/Icon.h"
#include "theme/Palette.h"
#include "catalog/containers/Tabs.h"
#include <algorithm>
#include <common.h>
#include <qlogging.h>
#include <qtabbar.h>
#include <qtabwidget.h>
#include <qwidget.h>

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

    m_tab->setTabsClosable(true);
    m_tab->setMovable(true);

    // Floated over the tab widget rather than installed as its corner widget: the
    // corner geometry is derived from the widget's size hint while the stylesheet
    // paints it larger, which left the button laid out past the window's right edge.
    m_settings_btn = new catalog::Button(catalog::ButtonRole::InlineIcon, m_tab);
    m_settings_btn->setIcon(icon::settings());
    m_settings_btn->setIconSize(QSize(metric::ICON_SIZE, metric::ICON_SIZE));
    // Sized to sit inside the tab strip rather than overhang it.
    m_settings_btn->setFixedSize(metric::ICON_SIZE + 2 * resolve(Padding::XXS),
                                 metric::ICON_SIZE + 2 * resolve(Padding::XXS));
    m_settings_btn->setToolTip(TR("settings-title"));
    connect(m_settings_btn, &QPushButton::clicked, AppController::get(),
            &AppController::openAppSettings, qontrol::UNIQUE);

    updateTabs();

    setCentralWidget(m_tab);
    positionSettingsButton();

    connect(m_tab, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested,
            qontrol::UNIQUE);

    m_init = true;
}

auto MainWindow::positionSettingsButton() -> void {
    if (m_settings_btn == nullptr || m_tab == nullptr) {
        return;
    }
    // Top-right of the tab strip, vertically centred on the tab bar.
    int barHeight = m_tab->tabBar()->height();
    int top = barHeight > m_settings_btn->height() ? (barHeight - m_settings_btn->height()) / 2 : 0;
    m_settings_btn->move(m_tab->width() - m_settings_btn->width() - resolve(Spacing::XS), top);
    m_settings_btn->raise();
}

auto MainWindow::resizeEvent(QResizeEvent *event) -> void {
    qontrol::Window::resizeEvent(event);
    positionSettingsButton();
}

auto MainWindow::updateTabs() -> void {
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

    // Adding tabs resizes the tab bar, which the floating button is centred on.
    positionSettingsButton();
}

auto MainWindow::tabExists(QWidget *content) const -> bool {
    return std::ranges::any_of(m_tabs, [content](const auto &tab) { return tab.second == content; });
}

auto MainWindow::addTab(QWidget *content, const QString &title) -> int {
    if (content == nullptr || tabExists(content)) {
        return -1;
    }
    m_tabs.append(QPair<QString, QWidget *>(title, content));
    updateTabs();
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
    if (m_settings_btn != nullptr) {
        m_settings_btn->setToolTip(TR("settings-title"));
    }
}

auto MainWindow::closeEvent(QCloseEvent *event) -> void {
    auto *host = Host::get();
    if (host != nullptr) {
        auto activeInstances = host->instances();
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
    if (hosted != nullptr && hosted != m_menu_tab) {
        emit hostedTabCloseRequested(hosted);
    }
}

MainWindow::~MainWindow() = default;
