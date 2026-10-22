#pragma once

#include <QWidget>

class AccountController;

namespace account {
class AccountShell;
}

namespace qontrol {
class Panel;
}

class AccountWidget : public QWidget {
    Q_OBJECT

public:
    explicit AccountWidget(const QString &account, QWidget *parent = nullptr);
    ~AccountWidget() override;

    auto controller() -> AccountController *;
    void loadPanel(qontrol::Panel *panel);

protected:
    void initUI();

private:
    AccountController *m_controller = nullptr;
    account::AccountShell *m_shell = nullptr;
};
