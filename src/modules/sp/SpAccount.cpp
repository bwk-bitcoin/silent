#include "SpAccount.h"

#include "AccountController.h"

#include <QMap>
#include <QString>
#include <common.h>
#include <silent.h>
#include <optional>
#include <string>

namespace {
auto toQString(rust::String value) -> QString {
    return QString::fromStdString(std::string(value.c_str()));
}

auto toPluginCoin(RustCoin coin) -> plugin::Coin {
    plugin::Coin out;
    out.outpoint = toQString(std::move(coin.outpoint));
    out.value = coin.value;
    out.height = coin.height;
    out.label = toQString(std::move(coin.label));
    out.spent = coin.spent;
    out.accountType = toQString(std::move(coin.account_type));
    return out;
}
} // namespace

SpAccount::SpAccount(AccountController *controller) : m_controller(controller) {
    if (m_controller != nullptr) {
        connect(m_controller, &AccountController::updateCoins, this, &SpAccount::onControllerCoinsUpdate,
                qontrol::UNIQUE);
        connect(m_controller, &AccountController::updateBalance, this,
                &SpAccount::onControllerBalanceUpdate, qontrol::UNIQUE);
    }
}

bool SpAccount::implemented() const {
    return true;
}

ReqId SpAccount::name() {
    ReqId reqId = nextReqId();
    if (!accountReady(reqId)) {
        return reqId;
    }

    auto rustName = m_controller->getAccount().value()->name();
    emit IAccount::name(reqId, toQString(std::move(rustName)));
    return reqId;
}

ReqId SpAccount::info() {
    ReqId reqId = nextReqId();
    if (!accountReady(reqId)) {
        return reqId;
    }

    QMap<QString, QString> out;
    auto rustName = m_controller->getAccount().value()->name();
    out.insert(QStringLiteral("kind"), QStringLiteral("sp"));
    out.insert(QStringLiteral("displayName"), toQString(std::move(rustName)));
    emit IAccount::info(reqId, out);
    return reqId;
}

ReqId SpAccount::coins() {
    ReqId reqId = nextReqId();
    emitCoins(reqId);
    return reqId;
}

ReqId SpAccount::balance() {
    ReqId reqId = nextReqId();
    emitBalance(reqId);
    return reqId;
}

ReqId SpAccount::newReceiveAddress() {
    ReqId reqId = nextReqId();
    if (!accountReady(reqId)) {
        return reqId;
    }

    auto addr = m_controller->getAccount().value()->new_segwit_addr();
    emit receiveAddress(reqId, toQString(std::move(addr)));
    return reqId;
}

ReqId SpAccount::newChangeAddress() {
    ReqId reqId = nextReqId();
    if (!accountReady(reqId)) {
        return reqId;
    }

    auto addr = m_controller->getAccount().value()->new_taproot_addr();
    emit changeAddress(reqId, toQString(std::move(addr)));
    return reqId;
}

ReqId SpAccount::descriptor() {
    ReqId reqId = nextReqId();
    if (!accountReady(reqId)) {
        return reqId;
    }
    emit IAccount::descriptor(reqId, toQString(m_controller->getAccount().value()->descriptor()));
    return reqId;
}

ReqId SpAccount::raw(const QByteArray &request) {
    Q_UNUSED(request);
    ReqId reqId = nextReqId();
    emit error(reqId, QStringLiteral("SP account raw() not implemented"));
    emit IAccount::raw(reqId, QByteArray());
    return reqId;
}

void SpAccount::onControllerCoinsUpdate() {
    emitCoins(0);
}

void SpAccount::onControllerBalanceUpdate(uint64_t balance) {
    Q_UNUSED(balance);
    emitBalance(0);
}

bool SpAccount::accountReady(ReqId req_id) {
    if (m_controller == nullptr || !m_controller->getAccount().has_value()) {
        emit error(req_id, QStringLiteral("SP account not initialized"));
        return false;
    }
    return true;
}

ReqId SpAccount::nextReqId() {
    ++m_next_req_id;
    if (m_next_req_id == 0) {
        ++m_next_req_id;
    }
    return m_next_req_id;
}

void SpAccount::emitCoins(ReqId req_id) {
    QList<plugin::Coin> out;
    if (m_controller == nullptr) {
        emit error(req_id == 0 ? std::nullopt : std::optional<ReqId>(req_id),
                   QStringLiteral("SP account controller missing"));
        return;
    }

    auto rustCoins = m_controller->getCoins();
    out.reserve(static_cast<qsizetype>(rustCoins.size()));
    for (const auto &coin : rustCoins) {
        out.append(toPluginCoin(coin));
    }
    emit IAccount::coins(req_id, out);
}

void SpAccount::emitBalance(ReqId req_id) {
    if (m_controller == nullptr || !m_controller->getAccount().has_value()) {
        emit error(req_id == 0 ? std::nullopt : std::optional<ReqId>(req_id),
                   QStringLiteral("SP account not initialized"));
        return;
    }

    auto state = m_controller->getAccount().value()->spendable_coins();
    plugin::Balance out{};
    out.confirmed = state.confirmed_balance;
    out.unconfirmed = state.unconfirmed_balance;
    emit IAccount::balance(req_id, out);
}
