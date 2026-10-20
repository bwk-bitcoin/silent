#pragma once

#include <optional>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <interfaces/types.h>

class IFeed : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~IFeed() override = default;

    // Signals may borrow plugin literals for the duration of emit. The host copies
    // payloads directly before queueing. Instance stop must join all emitters
    // before returning; the library cannot unload while its code is executing.

    [[nodiscard]] virtual bool implemented() const = 0;
    virtual ReqId raw(const QByteArray &request) = 0;

    // Empty defaults mean this feed has no price capability. Connect before requesting.
    virtual auto currencies() -> ReqId {
        return 0;
    }

    virtual auto requestRates(const QStringList &currencies) -> ReqId {
        Q_UNUSED(currencies);
        return 0;
    }

    // Discover first, then request only supported recommendations. Feeds push updates.
    virtual auto feeCapabilities() -> ReqId {
        return 0;
    }

    virtual auto requestDefaultFeeRate() -> ReqId {
        return 0;
    }

    virtual auto requestSmartFeeRates() -> ReqId {
        return 0;
    }

signals:
    // Non-positive or non-finite values withdraw the matching recommendation.
    void defaultFeeRate(ReqId req_id, double default_satvb);
    void smartFeeRates(ReqId req_id, double low_satvb, double medium_satvb, double fast_satvb);
    void feeCapabilities(ReqId req_id, bool has_default, bool has_smart);
    void currencies(ReqId req_id, QStringList currencies);
    // Fiat units per BTC. Non-positive or non-finite values withdraw this quote.
    void conversionRate(ReqId req_id, QString currency, double value);
    void raw(ReqId req_id, QByteArray response);
    void error(std::optional<ReqId> req_id, QString message);
};

Q_DECLARE_INTERFACE(IFeed, "dev.silent.IFeed/3.0")
