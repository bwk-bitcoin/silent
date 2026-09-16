#pragma once

#include <memory>

#include <QString>

#include <interfaces/instance.h>

class HwiModule;

// Headless instance with no UI of its own. It exists so the device service is started
// and auto restarted, the way any other instance is.
class HwiServiceInstance final : public IInstance {
public:
    HwiServiceInstance(HwiModule *module, QString instance_id);
    ~HwiServiceInstance() override;

    [[nodiscard]] QString id() const override;
    void stop() override;

    IAccount *account() override;
    IFeed *feed() override;
    ISigner *signer() override;
    IThemeProvider *theme() override;

private:
    QString m_id;
    std::unique_ptr<IAccount> m_account;
    std::unique_ptr<IFeed> m_feed;
    std::unique_ptr<ISigner> m_signer;
    std::unique_ptr<IThemeProvider> m_theme;
    bool m_stopped = false;
};
