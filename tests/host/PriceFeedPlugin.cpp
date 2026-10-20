#include "interfaces/feed.h"
#include <thread>

// Keep fixture-only metatypes and literals local so dlclose actually unmaps them.
class Q_DECL_HIDDEN PriceFeedPlugin final : public IFeed {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "dev.silent.tests.PriceFeed/1.0")

public:
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
        });
        emitter.join();
        return 2;
    }

private:
    ReqId m_requests = 0;
};

#include "PriceFeedPlugin.moc"
