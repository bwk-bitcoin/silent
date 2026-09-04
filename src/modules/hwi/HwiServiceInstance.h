#pragma once

#include <memory>

#include <QPointer>
#include <QString>

#include <interfaces/host.h>
#include <interfaces/instance.h>

class HwiModule;
class HwiSettings;

// Headless instance whose only job is to own the settings page. It exists so the page
// is there before any device is plugged in: UI is pushed by an instance, not a module.
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
    QPointer<HwiSettings> m_settings;
    Host::SectionId m_section_id = 0;
    std::unique_ptr<IAccount> m_account;
    std::unique_ptr<IFeed> m_feed;
    std::unique_ptr<ISigner> m_signer;
    std::unique_ptr<IThemeProvider> m_theme;
    bool m_stopped = false;
};
