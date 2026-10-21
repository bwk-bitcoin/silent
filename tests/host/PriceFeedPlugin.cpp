#include "interfaces/feed.h"
#include "interfaces/module.h"
#include <thread>

// Keep fixture-only metatypes and literals local so dlclose actually unmaps them.
class Q_DECL_HIDDEN PriceFeedPlugin final : public IFeed, public IPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SILENT_IPlugin_IID)
    Q_INTERFACES(IPlugin)

public:
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {};
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
        return {};
    }

    [[nodiscard]] auto implemented() const -> bool override {
        return true;
    }

    auto currencies() -> ReqId override {
        emit IFeed::currencies(1, {QStringLiteral("EUR")});
        return 1;
    }

    auto requestRates([[maybe_unused]] const QStringList &currencies) -> ReqId override {
        ++m_requests;
        emit conversionRate(1, QStringLiteral("EUR"), 50000);
        return 1;
    }

    auto feeCapabilities() -> ReqId override {
        emit IFeed::feeCapabilities(3, true, true);
        return 3;
    }

    auto requestDefaultFeeRate() -> ReqId override {
        emit defaultFeeRate(4, 0.5);
        return 4;
    }

    auto requestSmartFeeRates() -> ReqId override {
        emit smartFeeRates(5, 0.25, 0.5, 0.75);
        return 5;
    }

    auto raw(const QByteArray &request) -> ReqId override {
        if (request == "literal-address") {
            return reinterpret_cast<quintptr>(QStringLiteral("EUR").constData());
        }
        if (request == "requests") {
            return m_requests;
        }
        std::thread emitter([this]() {
            emit IFeed::currencies(2, {QStringLiteral("USD")});
            emit conversionRate(2, QStringLiteral("EUR"), 12345);
            emit IFeed::feeCapabilities(6, false, false);
            emit defaultFeeRate(7, 99);
            emit smartFeeRates(8, 98, 99, 100);
        });
        emitter.join();
        return 2;
    }

private:
    ReqId m_requests = 0;
};

#include "PriceFeedPlugin.moc"
