#pragma once

#include "interfaces/instance.h"

#include "interfaces/host.h"

#include <QPointer>
#include <QString>
#include <memory>

class AccountController;
class AccountWidget;
class IAccount;
class IFeed;
class ISigner;
class IThemeProvider;

class SpInstance final : public IInstance {
public:
    explicit SpInstance(QString id);

    [[nodiscard]] QString id() const override;
    void stop() override;
    [[nodiscard]] auto backend() const -> AccountController *;

    IAccount *account() override;
    IFeed *feed() override;
    ISigner *signer() override;
    IThemeProvider *theme() override;

private:
    QString m_id;
    AccountWidget *m_account_widget = nullptr;
    QPointer<AccountController> m_account_controller;

    std::unique_ptr<IAccount> m_account;
    std::unique_ptr<IFeed> m_feed;
    std::unique_ptr<ISigner> m_signer;
    std::unique_ptr<IThemeProvider> m_theme;

    Host::TabId m_tab_id = 0;
    bool m_stopped = false;
};
