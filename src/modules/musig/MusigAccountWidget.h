#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>
#include <Qontrol>

class MusigAccountController;
class MusigStatusBar;

// Tab content: menu and panel container.
class MusigAccountWidget : public QWidget {
    Q_OBJECT

public:
    explicit MusigAccountWidget(const QString &account, QWidget *parent = nullptr);
    ~MusigAccountWidget() override;
    auto controller() -> MusigAccountController *;
    void loadPanel(qontrol::Panel *panel);

protected:
    void initUI();

private:
    MusigAccountController *m_controller = nullptr;
    qontrol::Column *m_menu = nullptr;
    QWidget *m_screen_container = nullptr;
    QPointer<qontrol::Panel> m_current_panel;
    MusigStatusBar *m_status_bar = nullptr;
};
