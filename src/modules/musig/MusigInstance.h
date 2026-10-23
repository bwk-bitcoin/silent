#pragma once

#include "interfaces/host.h"
#include "interfaces/instance.h"

#include <QPointer>
#include <QString>
#include <memory>

class MusigAccountController;
class MusigAccountWidget;

class MusigInstance final : public IInstance {
public:
    explicit MusigInstance(QString id);
    auto openPage() -> bool;
    [[nodiscard]] auto id() const -> QString override;
    void stop() override;
    [[nodiscard]] auto backend() const -> MusigAccountController *;
    auto account() -> IAccount * override;
    auto feed() -> IFeed * override;           // nullptr
    auto signer() -> ISigner * override;       // nullptr
    auto theme() -> IThemeProvider * override; // nullptr

private:
    QString m_id;
    MusigAccountWidget *m_account_widget = nullptr;
    QPointer<MusigAccountController> m_account_controller;
    std::unique_ptr<IAccount> m_account;
    Host::TabId m_tab_id = 0;
    bool m_stopped = false;
};
