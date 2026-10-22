#pragma once

#include <QString>
#include <QWidget>

class MusigAccountController;

namespace account {
class AccountShell;
}

namespace qontrol {
class Panel;
}

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
    account::AccountShell *m_shell = nullptr;
};
