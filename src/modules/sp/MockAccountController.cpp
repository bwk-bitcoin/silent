#include "MockAccountController.h"

#include "SpSigning.h"
#include "account/services/ServiceAdapters.h"
#include "account/services/ServiceContext.h"
#include "account/views/Coins.h"
#include "account/views/History.h"
#include "account/views/Receive.h"
#include "account/views/Send.h"
#include "account/views/Settings.h"
#include "interfaces/backend/account.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "interfaces/backend/settings.h"
#include "interfaces/backend/transaction.h"
#include "modules/sp/SpModule.h"

#include <QString>
#include <Qontrol>

namespace {

const quint64 MOCK_FEE = 1000;

auto dummyCoin(const char *type, const char *outpoint, const char *label, quint64 value,
               quint64 height) -> backend::Coin {
    return {.outpoint = QString::fromLatin1(outpoint),
            .amount = value,
            .height = height == 0 ? std::nullopt : std::optional<quint64>(height),
            .label = QString::fromLatin1(label),
            .origin = QString::fromLatin1(type),
            .spendable = true};
}

auto dummyTx(const char *direction, const char *txid, quint64 amount, quint64 height)
    -> backend::Transaction {
    return {.txid = QString::fromLatin1(txid),
            .direction = QString::fromLatin1(direction),
            .amount = amount,
            .height = height == 0 ? std::nullopt : std::optional<quint64>(height)};
}

// Dummy but valid funds and crafting, so every panel renders in debug mode. Requests
// with nothing to fake answer failed().
class MockServices final : public account::ServiceContext {
public:
    explicit MockServices(QObject *parent) : ServiceContext(QStringLiteral("mock"), parent) {
        publishNetwork(backend::Network::Regtest);
        publishSettings({.network = backend::Network::Regtest,
                         .endpoints = {{.id = QStringLiteral("blindbit"), .name = "BlindBit"},
                                       {.id = QStringLiteral("electrum"), .name = "Electrum"}}});
        publishState({.connections = {{.id = QStringLiteral("blindbit"), .name = "BlindBit"},
                                      {.id = QStringLiteral("electrum"), .name = "Electrum"}}});
        onFundsChanged();
        onReceivingChanged();
    }

    // The given coins, or just enough of them for the outputs plus the fee (all of them
    // for a MAX output), so the coin table highlights a realistic subset.
    auto craft(const backend::TransactionTemplate &transaction) -> backend::CraftResult override {
        auto result = transaction;
        bool sendMax = false;
        quint64 target = MOCK_FEE;
        for (const auto &output : transaction.outputs) {
            target += output.amount;
            sendMax = sendMax || output.maximum;
        }
        quint64 total = 0;
        for (const auto &coin : coins()) {
            bool picked = transaction.inputs.contains(coin.outpoint);
            if (transaction.inputs.isEmpty() && (sendMax || total < target)) {
                result.inputs.append(coin.outpoint);
                picked = true;
            }
            total += picked ? coin.amount : 0;
        }
        for (auto &output : result.outputs) {
            if (output.maximum) {
                output.amount = total > target ? total - target : 0;
            }
        }
        result.fee = MOCK_FEE;
        return {.transaction = result, .completion = {.complete = total >= target}};
    }

    auto finalize([[maybe_unused]] const backend::TransactionTemplate &transaction)
        -> std::optional<QByteArray> override {
        return std::nullopt;
    }

    void selectCoins(backend::RequestId token, backend::TransactionTemplate transaction) override {
        transaction.inputs.clear();
        emit coinsSelected(token, craft(transaction));
    }

    void newAddress(backend::RequestId token, QString kind) override {
        ++m_generated;
        auto prefix = kind == QStringLiteral("segwit") ? QStringLiteral("bc1qmocksegwit%1")
                                                       : QStringLiteral("bc1pmocktaproot%1");
        auto length = kind == QStringLiteral("segwit") ? 42 : 62;
        emit addressCreated(
            token, {.kind = kind,
                    .address = prefix.arg(m_generated).leftJustified(length, QLatin1Char('x'))});
    }

    void setLabel(backend::RequestId token, [[maybe_unused]] backend::LabelKey key,
                  [[maybe_unused]] QString text) override {
        emit failed(token, unsupported());
    }

    void fetchTransactionDetails(backend::RequestId token, [[maybe_unused]] QString txid) override {
        emit failed(token, unsupported());
    }

    void connectAll(backend::RequestId token, [[maybe_unused]] bool start) override {
        emit failed(token, unsupported());
    }

    void setConnected(backend::RequestId token, [[maybe_unused]] QString connection,
                      [[maybe_unused]] bool connected) override {
        emit failed(token, unsupported());
    }

    void refreshTip(backend::RequestId token, [[maybe_unused]] QString connection) override {
        emit failed(token, unsupported());
    }

    void testEndpoint(backend::RequestId token,
                      [[maybe_unused]] backend::Endpoint endpoint) override {
        emit failed(token, unsupported());
    }

    void saveSettings(backend::RequestId token,
                      [[maybe_unused]] backend::Settings settings) override {
        emit failed(token, unsupported());
    }

    void endpointCapabilities(backend::RequestId token,
                              [[maybe_unused]] QString endpoint) override {
        emit failed(token, unsupported());
    }

    void broadcast(backend::RequestId token, [[maybe_unused]] QByteArray transaction) override {
        emit failed(token, unsupported());
    }

protected:
    auto startFundsRead() -> bool override {
        Funds funds{.balance = {.confirmed = 167500000,
                                .unconfirmed = 100000000,
                                .confirmedCount = 5,
                                .unconfirmedCount = 1}};
        funds.coins = {
            dummyCoin("segwit",
                      "3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b:0",
                      "salary", 12500000, 850123),
            dummyCoin("taproot",
                      "8f5a2c1e9b4d6f8a0c2e4b6d8f0a2c4e6b8d0f2a4c6e8b0d2f4a6c8e0b2d4f6a:1", "",
                      5000000, 849876),
            dummyCoin("segwit",
                      "1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d:0",
                      "savings", 100000000, 847000),
            dummyCoin("taproot",
                      "9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b3a2f1e0d9c8b7a6f5e4d3c2b1a0f9e8d:2", "tip",
                      250000, 848500),
            dummyCoin("segwit",
                      "2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e0f1a2b3c:0", "rent",
                      37500000, 846200),
            dummyCoin("taproot",
                      "f1e2d3c4b5a6978869504132f1e2d3c4b5a697886950413200ffeeddccbbaa99:1", "",
                      780000, 848900),
            dummyCoin("segwit",
                      "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899:3",
                      "cold storage", 64000000, 845300)};
        funds.history = {
            dummyTx("incoming", "3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b",
                    12500000, 850123),
            dummyTx("outgoing", "8f5a2c1e9b4d6f8a0c2e4b6d8f0a2c4e6b8d0f2a4c6e8b0d2f4a6c8e0b2d4f6a",
                    5000000, 849876),
            dummyTx("incoming", "1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d",
                    100000000, 0),
            dummyTx("outgoing", "9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b3a2f1e0d9c8b7a6f5e4d3c2b1a0f9e8d",
                    250000, 848500),
            dummyTx("incoming", "0f1e2d3c4b5a6978f8e9d0c1b2a3948576f5e4d3c2b1a09f8e7d6c5b4a3f2e1d",
                    99999999, 847000)};
        fundsRead(funds);
        return true;
    }

    auto readLabels() -> std::optional<QList<backend::Label>> override {
        return QList<backend::Label>{};
    }

    auto readReceiving() -> std::optional<Receiving> override {
        Receiving receiving{
            .kinds = {{.id = QStringLiteral("sp"), .name = "Silent Payment", .reusable = true},
                      {.id = QStringLiteral("segwit"), .name = "Segwit"},
                      {.id = QStringLiteral("taproot"), .name = "Taproot"}},
            .addresses = {{.kind = QStringLiteral("sp"),
                           .address = QStringLiteral("sp1qqw3w7z8x9y0a1b2c3d4e5f6g7h8i9j0k1l2m3n4o"
                                                     "5p6q7r8s9t0u1v2w3x4y5z6dummyaddr")}}};
        return receiving;
    }

    static auto unsupported() -> QString {
        return QStringLiteral("The mock account does not do this");
    }

private:
    int m_generated = 0;
};

} // namespace

MockAccountController::MockAccountController(AccountWidget *widget)
    : AccountController(widget),
      m_mock_services(new MockServices(this)) {
}

auto MockAccountController::network() -> Network {
    return Network::Regtest;
}

auto MockAccountController::getDescriptor() -> rust::String {
    return {};
}

// The adapters over the mock context stand for a host assembly: the mock has no module
// account to assemble.
auto MockAccountController::loadPanels() -> void {
    auto adapter = [this](backend::ServiceKind kind) {
        return account::serviceAdapter(kind, m_mock_services, this);
    };
    account::Services services{
        .account = qobject_cast<backend::IAccountBackend *>(adapter(backend::ServiceKind::Account)),
        .blockchain =
            qobject_cast<backend::IBlockchain *>(adapter(backend::ServiceKind::Blockchain)),
        .transaction =
            qobject_cast<backend::ITransaction *>(adapter(backend::ServiceKind::Transaction)),
        .labels = qobject_cast<backend::ILabels *>(adapter(backend::ServiceKind::Labels)),
        .settings =
            qobject_cast<backend::IBackendSettings *>(adapter(backend::ServiceKind::Settings))};
    insertPanel(new qontrol::Panel(new account::view::Settings(services, nullptr), "settings"));
    insertPanel(new qontrol::Panel(new account::view::History(services), "history"));
    insertPanel(new qontrol::Panel(new account::view::Coins(services), "coins"));
    insertPanel(
        new qontrol::Panel(new account::view::Receive(services, SpModule::get()), "receive"));
    insertPanel(new qontrol::Panel(new account::view::Send(services, new SpSigning(this, services)),
                                   "send"));
}

// Surface the segwit and taproot sections in the debug Receive view.
auto MockAccountController::hasSegwit() -> bool {
    return true;
}

auto MockAccountController::hasTaproot() -> bool {
    return true;
}
