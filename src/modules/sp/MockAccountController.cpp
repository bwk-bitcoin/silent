#include "MockAccountController.h"

#include <QString>
#include <utility>

namespace {

auto dummyCoin(const char *type, const char *outpoint, const char *label, uint64_t value,
               uint32_t height) -> RustCoin {
    RustCoin coin;
    coin.account_type = rust::String(type);
    coin.outpoint = rust::String(outpoint);
    coin.label = rust::String(label);
    coin.value = value;
    coin.height = height;
    coin.spent = false;
    return coin;
}

auto dummyTx(const char *direction, const char *txid, uint64_t amount, uint32_t height) -> RustTx {
    RustTx tx;
    tx.txid = rust::String(txid);
    tx.direction = rust::String(direction);
    tx.amount = amount;
    tx.height = height;
    return tx;
}

} // namespace

MockAccountController::MockAccountController(AccountWidget *widget) : AccountController(widget) {
}

auto MockAccountController::loadPanels() -> void {
    AccountController::loadPanels(); // builds and connects the views

    // Push a dummy balance so the BalanceHeader populates once views are wired.
    CoinState state;
    state.confirmed_count = 5;
    state.confirmed_balance = 167500000;
    state.unconfirmed_count = 1;
    state.unconfirmed_balance = 100000000;
    emit updateCoins(state);
}

auto MockAccountController::getCoins() -> rust::Vec<RustCoin> {
    rust::Vec<RustCoin> coins;
    coins.push_back(dummyCoin("segwit",
                              "3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b:0",
                              "salary", 12500000, 850123));
    coins.push_back(dummyCoin("taproot",
                              "8f5a2c1e9b4d6f8a0c2e4b6d8f0a2c4e6b8d0f2a4c6e8b0d2f4a6c8e0b2d4f6a:1",
                              "", 5000000, 849876));
    coins.push_back(dummyCoin("segwit",
                              "1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d:0",
                              "savings", 100000000, 847000));
    coins.push_back(dummyCoin("taproot",
                              "9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b3a2f1e0d9c8b7a6f5e4d3c2b1a0f9e8d:2",
                              "tip", 250000, 848500));
    coins.push_back(dummyCoin("segwit",
                              "2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e0f1a2b3c:0",
                              "rent", 37500000, 846200));
    coins.push_back(dummyCoin("taproot",
                              "f1e2d3c4b5a6978869504132f1e2d3c4b5a697886950413200ffeeddccbbaa99:1",
                              "", 780000, 848900));
    coins.push_back(dummyCoin("segwit",
                              "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899:3",
                              "cold storage", 64000000, 845300));
    return coins;
}

auto MockAccountController::getPaymentHistory() -> rust::Vec<RustTx> {
    rust::Vec<RustTx> txs;
    txs.push_back(dummyTx("incoming",
                          "3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b",
                          12500000, 850123));
    txs.push_back(dummyTx("outgoing",
                          "8f5a2c1e9b4d6f8a0c2e4b6d8f0a2c4e6b8d0f2a4c6e8b0d2f4a6c8e0b2d4f6a",
                          5000000, 849876));
    txs.push_back(dummyTx("incoming",
                          "1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d",
                          100000000, 0));
    txs.push_back(dummyTx("outgoing",
                          "9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b3a2f1e0d9c8b7a6f5e4d3c2b1a0f9e8d",
                          250000, 848500));
    txs.push_back(dummyTx("incoming",
                          "0f1e2d3c4b5a6978f8e9d0c1b2a3948576f5e4d3c2b1a09f8e7d6c5b4a3f2e1d",
                          99999999, 847000));
    return txs;
}

auto MockAccountController::getSpAddress() -> rust::String {
    return rust::String(
        "sp1qqw3w7z8x9y0a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6q7r8s9t0u1v2w3x4y5z6dummyaddr");
}

auto MockAccountController::newSegwitAddr() -> rust::String {
    // A fresh, distinct dummy address each call so the generate/history flow can
    // be exercised offline. Padded to a realistic segwit length.
    m_segwit_index++;
    QString addr = QString("bc1qmocksegwit%1").arg(m_segwit_index).leftJustified(42, 'x');
    return rust::String(addr.toStdString());
}

auto MockAccountController::newTaprootAddr() -> rust::String {
    m_taproot_index++;
    QString addr = QString("bc1pmocktaproot%1").arg(m_taproot_index).leftJustified(62, 'x');
    return rust::String(addr.toStdString());
}

// Surface the segwit and taproot sections in the debug Receive view.
auto MockAccountController::hasSegwit() -> bool {
    return true;
}

auto MockAccountController::hasTaproot() -> bool {
    return true;
}

auto MockAccountController::simulateTx(TransactionTemplate tx) -> TransactionSimulation {
    // Dummy-but-valid simulation so the transaction flow renders in debug mode.
    // Select only enough coins to cover the outputs plus the fee (a realistic
    // subset), so the coin table highlights the selected rows rather than every
    // row. A max send takes everything.
    TransactionSimulation sim;
    sim.is_valid = true;
    sim.fee = 1000;
    sim.weight = 500;

    bool sendMax = false;
    uint64_t target = sim.fee;
    for (const auto &out : tx.outputs) {
        target += out.amount;
        if (out.max) {
            sendMax = true;
        }
    }

    uint64_t total = 0;
    auto coins = getCoins();
    for (auto &coin : coins) {
        if (!sendMax && total >= target) {
            break;
        }
        total += coin.value;
        sim.selected_outpoints.push_back(std::move(coin.outpoint));
    }
    sim.input_total = total;
    sim.input_count = sim.selected_outpoints.size();
    sim.output_total = total > sim.fee ? total - sim.fee : 0;
    return sim;
}
