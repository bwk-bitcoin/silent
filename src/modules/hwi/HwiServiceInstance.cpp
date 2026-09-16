#include "HwiServiceInstance.h"

#include "interfaces/account.h"
#include "interfaces/feed.h"
#include "interfaces/signer.h"
#include "interfaces/theme.h"

namespace {
// The service instance carries no data capability: it exists so the device service is
// started and auto restarted.
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

class InertSigner final : public ISigner {
public:
    using ISigner::ISigner;

    [[nodiscard]] bool implemented() const override { return false; }
    [[nodiscard]] QString id() const override { return QString(); }
    [[nodiscard]] QString fingerprint() const override { return QString(); }
    [[nodiscard]] QString walletName() const override { return QString(); }
    [[nodiscard]] QString brand() const override { return QString(); }
    [[nodiscard]] QString model() const override { return QString(); }
    [[nodiscard]] QString serial() const override { return QString(); }
    [[nodiscard]] plugin::SignerState state() const override {
        return plugin::SignerState::Unsupported;
    }
    [[nodiscard]] QString stateDetail() const override { return QString(); }
    ReqId init() override { return 0; }
    ReqId info() override { return 0; }
    ReqId getXpub(const QString &path) override {
        Q_UNUSED(path);
        return 0;
    }
    ReqId isDescriptorRegistered(const QString &descriptor) override {
        Q_UNUSED(descriptor);
        return 0;
    }
    ReqId registerDescriptor(const QString &descriptor) override {
        Q_UNUSED(descriptor);
        return 0;
    }
    ReqId signWithDescriptor(const QString &psbt_base64, const QString &descriptor) override {
        Q_UNUSED(psbt_base64);
        Q_UNUSED(descriptor);
        return 0;
    }
    ReqId displayAddress(const QString &descriptor, const QString &path,
                         const QString &address) override {
        Q_UNUSED(descriptor);
        Q_UNUSED(path);
        Q_UNUSED(address);
        return 0;
    }
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

HwiServiceInstance::HwiServiceInstance(HwiModule *module, QString instance_id)
    : m_id(std::move(instance_id))
    , m_account(std::make_unique<InertAccount>())
    , m_feed(std::make_unique<InertFeed>())
    , m_signer(std::make_unique<InertSigner>())
    , m_theme(std::make_unique<InertThemeProvider>()) {
    Q_UNUSED(module);
}

HwiServiceInstance::~HwiServiceInstance() {
    stop();
}

QString HwiServiceInstance::id() const {
    return m_id;
}

void HwiServiceInstance::stop() {
    if (m_stopped) {
        return;
    }
    m_stopped = true;

    // Nothing to tear down: the instance owns no UI and no device handle.
}

IAccount *HwiServiceInstance::account() {
    return m_account.get();
}

IFeed *HwiServiceInstance::feed() {
    return m_feed.get();
}

ISigner *HwiServiceInstance::signer() {
    return m_signer.get();
}

IThemeProvider *HwiServiceInstance::theme() {
    return m_theme.get();
}
