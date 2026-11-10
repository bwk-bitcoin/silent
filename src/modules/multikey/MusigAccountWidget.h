#pragma once

#include <QString>
#include <QWidget>

class MusigAccountController;

namespace account {
class AccountShell;
}

// Tab content: menu and panel container.
class MusigAccountWidget : public QWidget {
    Q_OBJECT

public:
    explicit MusigAccountWidget(const QString &account, QWidget *parent = nullptr);
    ~MusigAccountWidget() override;
    auto controller() -> MusigAccountController *;
    [[nodiscard]] auto shell() const -> account::AccountShell *;

protected:
    void initUI();

private:
    MusigAccountController *m_controller = nullptr;
    account::AccountShell *m_shell = nullptr;
};
