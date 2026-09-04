#pragma once

#include <memory>

#include <QString>

#include <interfaces/instance.h>

struct HwiDevice;
class HwiModule;
class HwiSigner;

// A single signing device. Only the signer capability is implemented; a device pushes
// no UI of its own, the module's settings page lists them all.
class HwiDeviceInstance final : public IInstance {
public:
    HwiDeviceInstance(HwiModule *module, const HwiDevice &device, QString instance_id);
    ~HwiDeviceInstance() override;

    [[nodiscard]] QString id() const override;
    void stop() override;

    IAccount *account() override;
    IFeed *feed() override;
    ISigner *signer() override;
    IThemeProvider *theme() override;

    [[nodiscard]] HwiSigner *hwiSigner() const;

private:
    QString m_id;
    std::unique_ptr<IAccount> m_account;
    std::unique_ptr<IFeed> m_feed;
    std::unique_ptr<HwiSigner> m_signer;
    std::unique_ptr<IThemeProvider> m_theme;
};
