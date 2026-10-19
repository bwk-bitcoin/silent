#include "HwiDeviceInstance.h"

#include "HwiSigner.h"

#include "interfaces/account.h"
#include "interfaces/feed.h"
#include "interfaces/theme.h"

namespace {
// A device has a signer and nothing else; the rest stay inert.
class InertAccount final : public IAccount {
public:
    using IAccount::IAccount;

    [[nodiscard]] bool implemented() const override { return false; }
    ReqId name() override { return 0; }
    ReqId info() override { return 0; }
    ReqId coins() override { return 0; }
    ReqId balance() override { return 0; }
    ReqId newReceiveAddress() override { return 0; }
    ReqId newChangeAddress() override { return 0; }
    ReqId descriptor() override { return 0; }
    ReqId raw(const QByteArray &request) override {
        Q_UNUSED(request);
        return 0;
    }
};

class InertFeed final : public IFeed {
public:
    using IFeed::IFeed;

    [[nodiscard]] bool implemented() const override { return false; }
    ReqId raw(const QByteArray &request) override {
        Q_UNUSED(request);
        return 0;
    }
};

class InertThemeProvider final : public IThemeProvider {
public:
    using IThemeProvider::IThemeProvider;

    [[nodiscard]] bool implemented() const override { return false; }
    ReqId themes() override { return 0; }
    ReqId palette(const QString &name) override {
        Q_UNUSED(name);
        return 0;
    }
    ReqId raw(const QByteArray &request) override {
        Q_UNUSED(request);
        return 0;
    }
};
} // namespace

HwiDeviceInstance::HwiDeviceInstance(HwiModule *module, const HwiDevice &device,
                                     QString instance_id)
    : m_id(std::move(instance_id))
    , m_account(std::make_unique<InertAccount>())
    , m_feed(std::make_unique<InertFeed>())
    , m_signer(std::make_unique<HwiSigner>(module, device))
    , m_theme(std::make_unique<InertThemeProvider>()) {}

HwiDeviceInstance::~HwiDeviceInstance() = default;

QString HwiDeviceInstance::id() const {
    return m_id;
}

void HwiDeviceInstance::stop() {
    // Nothing to tear down: the device belongs to the service, not to this instance,
    // and no UI was pushed.
}

IAccount *HwiDeviceInstance::account() {
    return m_account.get();
}

IFeed *HwiDeviceInstance::feed() {
    return m_feed.get();
}

ISigner *HwiDeviceInstance::signer() {
    return m_signer.get();
}

IThemeProvider *HwiDeviceInstance::theme() {
    return m_theme.get();
}

HwiSigner *HwiDeviceInstance::hwiSigner() const {
    return m_signer.get();
}
