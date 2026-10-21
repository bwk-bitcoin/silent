#pragma once

#include <interfaces/signer.h>

namespace smoke {

// Public SDK only: no HWI provider or application implementation headers.
class ExternalSigner final : public ISigner {
public:
    explicit ExternalSigner(plugin::SignerCapabilities capabilities)
        : m_capabilities(capabilities) {
    }

    [[nodiscard]] auto implemented() const -> bool override {
        return true;
    }

    [[nodiscard]] auto id() const -> QString override {
        return QStringLiteral("external");
    }

    [[nodiscard]] auto fingerprint() const -> QString override {
        return QStringLiteral("aabbccdd");
    }

    [[nodiscard]] auto walletName() const -> QString override {
        return {};
    }

    [[nodiscard]] auto brand() const -> QString override {
        return QStringLiteral("fixture");
    }

    [[nodiscard]] auto model() const -> QString override {
        return {};
    }

    [[nodiscard]] auto serial() const -> QString override {
        return {};
    }

    [[nodiscard]] auto state() const -> plugin::SignerState override {
        return plugin::SignerState::Ready;
    }

    [[nodiscard]] auto stateDetail() const -> QString override {
        return {};
    }

    [[nodiscard]] auto capabilities() const -> plugin::SignerCapabilities override {
        return m_capabilities;
    }

    [[nodiscard]] auto signCalls() const -> int {
        return m_sign_calls;
    }

    auto init() -> ReqId override {
        return 0;
    }

    auto info() -> ReqId override {
        return 0;
    }

    auto getXpub([[maybe_unused]] const QString &path) -> ReqId override {
        return 0;
    }

    auto isDescriptorRegistered([[maybe_unused]] const QString &descriptor) -> ReqId override {
        return 0;
    }

    auto registerDescriptor([[maybe_unused]] const QString &descriptor) -> ReqId override {
        return 0;
    }

    auto signWithDescriptor([[maybe_unused]] const QString &psbt_base64,
                            [[maybe_unused]] const QString &descriptor) -> ReqId override {
        ++m_sign_calls;
        return 1;
    }

    auto displayAddress([[maybe_unused]] const QString &descriptor,
                        [[maybe_unused]] const QString &path,
                        [[maybe_unused]] const QString &address) -> ReqId override {
        return 0;
    }

    auto raw([[maybe_unused]] const QByteArray &request) -> ReqId override {
        return 0;
    }

private:
    plugin::SignerCapabilities m_capabilities;
    int m_sign_calls = 0;
};

} // namespace smoke
