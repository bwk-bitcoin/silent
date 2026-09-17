#pragma once

#include <QCloseEvent>
#include <QEvent>
#include <QList>
#include <QPair>
#include <QWidget>
#include <Qontrol>

namespace catalog {
class Button;
class Tabs;
}

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

signals:
    void hostedTabCloseRequested(QWidget *content);

public slots:
    auto onTabCloseRequested(int index) -> void;

protected:
    auto closeEvent(QCloseEvent *event) -> void override;
    auto changeEvent(QEvent *event) -> void override;
    auto resizeEvent(QResizeEvent *event) -> void override;

protected:
    auto initWindow() -> void;
    auto retranslateUi() -> void;
    auto positionSettingsButton() -> void;

private:
    bool m_init = false;
    catalog::Tabs *m_tab = nullptr;
    catalog::Button *m_settings_btn = nullptr;
    QList<QPair<QString, QWidget *>> m_tabs;
    QWidget *m_menu_tab = nullptr;
    QWidget *m_keys_tab = nullptr;
};
