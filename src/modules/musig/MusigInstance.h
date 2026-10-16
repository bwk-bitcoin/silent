#pragma once

#include "interfaces/host.h"
#include "interfaces/instance.h"

#include <QString>
#include <memory>

class MusigAccountController;
class MusigAccountWidget;

class MusigInstance final : public IInstance {
public:
    explicit MusigInstance(QString id);
    [[nodiscard]] auto id() const -> QString override;
    void stop() override;
    auto account() -> IAccount * override;
    auto feed() -> IFeed * override;           // nullptr
    auto signer() -> ISigner * override;       // nullptr
    auto theme() -> IThemeProvider * override; // nullptr

private:
    QString m_id;
    MusigAccountWidget *m_account_widget = nullptr;
    MusigAccountController *m_account_controller = nullptr;
    std::unique_ptr<IAccount> m_account;
    Host::TabId m_tab_id = 0;
    bool m_stopped = false;
};
