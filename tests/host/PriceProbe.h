#pragma once

#include "ViewServices.h"
#include "account/SendSigning.h"
#include "interfaces/backend/types.h"
#include "interfaces/feed.h"
#include "interfaces/host.h"
#include "interfaces/instance.h"
#include "interfaces/module.h"
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QWidget>
#include <optional>

namespace smoke {

class PriceEvents final : public QObject {
    Q_OBJECT

public:
    int sources = 0;
    int selected = 0;
    bool account_stopped = false;
    bool screens_destroyed = false;
    QList<QPointer<QObject>> account_views;

public slots:

    void onSourcesChanged() {
        ++sources;
    }

    void onSelectedChanged() {
        ++selected;
    }

    void onAccountStopped() {
        account_stopped = true;
    }

    void onAccountDestroyed() {
        screens_destroyed = true;
        for (const auto &view : account_views) {
            screens_destroyed = screens_destroyed && view.isNull();
        }
    }
};

class PriceFeed final : public IFeed {
    Q_OBJECT

public:
    bool active = true;
    bool deferred = false;
    int discoveries = 0;
    int requests = 0;
    QStringList offered{QStringLiteral("EUR"), QStringLiteral("USD")};
    QStringList requested;
    double value = 50000;
    bool has_default = false;
    bool has_smart = false;
    int fee_discoveries = 0;
    int default_requests = 0;
    int smart_requests = 0;
    double default_rate = 0.5;

    auto feeCapabilities() -> ReqId override {
        ++fee_discoveries;
        if (!deferred) {
            publishFeeCapabilities();
        }
        return 2;
    }

    void publishFeeCapabilities() {
        emit IFeed::feeCapabilities(2, has_default, has_smart);
    }

    auto requestDefaultFeeRate() -> ReqId override {
        ++default_requests;
        emit defaultFeeRate(3, default_rate);
        return 3;
    }

    auto requestSmartFeeRates() -> ReqId override {
        ++smart_requests;
        emit smartFeeRates(4, 0.25, 0.5, 0.75);
        return 4;
    }

    [[nodiscard]] auto implemented() const -> bool override {
        return active;
    }

    auto raw([[maybe_unused]] const QByteArray &request) -> ReqId override {
        return 0;
    }

    auto currencies() -> ReqId override {
        ++discoveries;
        if (!deferred) {
            publishCurrencies();
        }
        return 1;
    }

    auto requestRates(const QStringList &currencies) -> ReqId override {
        ++requests;
        requested = currencies;
        if (!deferred) {
            for (const auto &currency : currencies) {
                publish(currency, value);
            }
        }
        return 1;
    }

    void publishCurrencies() {
        emit IFeed::currencies(1, offered);
    }

    void publish(const QString &currency, double rate) {
        emit conversionRate(1, currency, rate);
    }
};

class PriceInstance final : public IInstance {
public:
    QString instanceId;
    IFeed *provider = nullptr;
    bool stopped = false;

    [[nodiscard]] auto id() const -> QString override {
        return instanceId;
    }

    void stop() override {
        stopped = true;
    }

    auto account() -> IAccount * override {
        return nullptr;
    }

    auto feed() -> IFeed * override {
        return provider;
    }

    auto signer() -> ISigner * override {
        return nullptr;
    }

    auto theme() -> IThemeProvider * override {
        return nullptr;
    }
};

class NonPriceFeed final : public IFeed {
public:
    [[nodiscard]] auto implemented() const -> bool override {
        return true;
    }

    auto raw([[maybe_unused]] const QByteArray &request) -> ReqId override {
        return 0;
    }
};

class PriceModule final : public IModule {
    Q_OBJECT

public:
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("price.module")},
                {QStringLiteral("name"), QStringLiteral("Price probe")}};
    }

    auto list() -> ReqId override {
        return 0;
    }

    auto autoStart() -> ReqId override {
        return 0;
    }

    void startInstance([[maybe_unused]] const QString &id) override {
    }
};

class PricePlugin final : public IPlugin {
public:
    PriceModule module;

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("price.plugin")}};
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
        return {const_cast<PriceModule *>(&module)};
    }
};

class PriceSigning final : public account::SendSigning {
public:
    int updates = 0;

    [[nodiscard]] auto actions() -> QWidget * override {
        return new QWidget;
    }

    [[nodiscard]] auto coinsWidget() -> QWidget * override {
        return nullptr;
    }

    void setTransaction([[maybe_unused]] const std::optional<backend::TransactionTemplate> &tx,
                        [[maybe_unused]] const QStringList &outpoints) override {
        ++updates;
    }

    [[nodiscard]] auto estimatesFee() const -> bool override {
        return true;
    }
};

// The services of a probe Send. Crafting spends 10000000 sats for a 2000 sats fee: a MAX
// payment takes what the fixed payments leave of 9998000 sats. Complete only once
// `valid_craft` is set.
class PriceBackend final {
public:
    static constexpr quint64 SPENDABLE = 9998000;
    static constexpr quint64 FEE = 2000;

    PriceBackend() {
        services.transaction.crafting = [this](const backend::TransactionTemplate &transaction) {
            return craft(transaction);
        };
    }

    // The signing step of the next Send, which takes it.
    auto newSigning() -> PriceSigning * {
        signing = new PriceSigning;
        return signing;
    }

    ViewServices services;
    PriceSigning *signing = nullptr;
    bool valid_craft = false;

protected:
    [[nodiscard]] auto craft(const backend::TransactionTemplate &transaction) const
        -> backend::CraftResult {
        backend::CraftResult result{.transaction = transaction,
                                    .completion = {.complete = valid_craft}};
        result.transaction.fee = FEE;
        quint64 fixedTotal = 0;
        for (const auto &output : transaction.outputs) {
            if (!output.maximum) {
                fixedTotal += output.amount;
            }
        }
        for (auto &output : result.transaction.outputs) {
            if (output.maximum && SPENDABLE > fixedTotal) {
                output.amount = SPENDABLE - fixedTotal;
            }
        }
        return result;
    }
};

auto priceFeedSmoke() -> bool;

} // namespace smoke
