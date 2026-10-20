#pragma once

#include "account/AccountBackend.h"
#include "account/SendSigning.h"
#include "interfaces/feed.h"
#include "interfaces/host.h"
#include "interfaces/instance.h"
#include "interfaces/module.h"
#include <QWidget>

namespace smoke {

class PriceEvents final : public QObject {
    Q_OBJECT

public:
    int sources = 0;
    int selected = 0;

public slots:

    void onSourcesChanged() {
        ++sources;
    }

    void onSelectedChanged() {
        ++selected;
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

    void setTransaction([[maybe_unused]] const std::optional<TransactionTemplate> &tx,
                        [[maybe_unused]] const QStringList &outpoints) override {
        ++updates;
    }

    [[nodiscard]] auto estimatesFee() const -> bool override {
        return true;
    }
};

class PriceBackend final : public AccountBackend {
public:
    int simulations = 0;
    bool validSimulation = false;
    PriceSigning *signing = nullptr;
    Network selected_network = Network::Regtest;

    auto getCoins() -> rust::Vec<RustCoin> override {
        return {};
    }

    auto getPaymentHistory() -> rust::Vec<RustTx> override {
        return {};
    }

    auto network() -> Network override {
        return selected_network;
    }

    auto module() -> IModule * override {
        return nullptr;
    }

    auto addressKinds() -> QList<AddressKind> override {
        return {};
    }

    auto newAddress([[maybe_unused]] AddressKind kind) -> QString override {
        return {};
    }

    auto addressVerifyInfo([[maybe_unused]] const QString &address) -> AddressVerifyInfo override {
        return {};
    }

    auto endpoints() -> QList<Endpoint> override {
        return {};
    }

    auto endpointUrl([[maybe_unused]] Endpoint endpoint) -> QString override {
        return {};
    }

    [[nodiscard]] auto verifiesCertificate() const -> bool override {
        return true;
    }

    [[nodiscard]] auto isEndpointRunning([[maybe_unused]] Endpoint endpoint) const
        -> bool override {
        return false;
    }

    auto startEndpoint([[maybe_unused]] Endpoint endpoint) -> bool override {
        return false;
    }

    void stopEndpoint([[maybe_unused]] Endpoint endpoint) override {
    }

    [[nodiscard]] auto networkEditable() const -> bool override {
        return false;
    }

    auto saveSettings([[maybe_unused]] const QHash<Endpoint, QString> &urls,
                      [[maybe_unused]] Network network) -> QString override {
        return {};
    }

    auto settingsExtras() -> QWidget * override {
        return nullptr;
    }

    auto simulateTx([[maybe_unused]] TransactionTemplate tx) -> TransactionSimulation override {
        ++simulations;
        TransactionSimulation result{};
        result.is_valid = validSimulation;
        result.output_total = 9998000;
        result.fee = 2000;
        return result;
    }

    auto sendSigning() -> account::SendSigning * override {
        signing = new PriceSigning;
        return signing;
    }

    void startWorker([[maybe_unused]] QThread *worker) override {
    }

    void updateCoinLabel([[maybe_unused]] const QString &outpoint,
                         [[maybe_unused]] const QString &label) override {
    }
};

auto priceFeedSmoke() -> bool;

} // namespace smoke
