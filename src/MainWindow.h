#pragma once

#include <QCloseEvent>
#include <QEvent>
#include <QList>
#include <QPair>
#include <QTabBar>
#include <QWidget>
#include <Qontrol>

namespace catalog {
class Tabs;
} // namespace catalog

class MainWindow : public qontrol::Window {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    auto addTab(QWidget *content, const QString &title) -> int;
    auto removeTab(QWidget *content, bool delete_content = true) -> void;
    auto setTabTitle(QWidget *content, const QString &title) -> void;
    auto tabExists(QWidget *content) const -> bool;
    auto updateTabs() -> void;
    void showKeysTab();

signals:
    void hostedTabCloseRequested(QWidget *content);

public slots:
    auto onTabCloseRequested(int index) -> void;
    void onCurrentTabChanged();
    void onModuleTabClicked(int index);

protected:
    auto closeEvent(QCloseEvent *event) -> void override;
    auto changeEvent(QEvent *event) -> void override;

protected:
    auto initWindow() -> void;
    auto retranslateUi() -> void;
    void updateModuleTabs();
    // The module pages, in the order of their tabs: Keys, then Settings.
    [[nodiscard]] auto modulePages() const -> QList<QPair<QWidget *, QString>>;

private:
    bool m_init = false;
    catalog::Tabs *m_tab = nullptr;
    // The module tabs (Keys, Settings), right aligned, apart from the accounts.
    QTabBar *m_module_bar = nullptr;
    QList<QPair<QString, QWidget *>> m_tabs;
    QWidget *m_menu_tab = nullptr;
    QWidget *m_keys_tab = nullptr;
    QWidget *m_settings_tab = nullptr;
};
