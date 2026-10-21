#include "modules/musig/MusigAccountAdapter.h"

#include "account/Spending.h"
#include "modules/musig/MusigAccountController.h"

#include <QList>
#include <QMap>
#include <QString>
#include <Qontrol>
#include <common.h>
#include <optional>
#include <silent.h>
#include <string>

namespace {
auto toQString(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}

// A push from the controller has no request: its error carries no id.
auto requestOf(ReqId req_id) -> std::optional<ReqId> {
    return req_id == 0 ? std::nullopt : std::optional<ReqId>(req_id);
}
} // namespace

MusigAccountAdapter::MusigAccountAdapter(MusigAccountController *controller)
    : m_controller(controller) {
    if (m_controller != nullptr) {
        connect(m_controller, &MusigAccountController::updateCoins, this,
                &MusigAccountAdapter::onControllerCoinsUpdate, qontrol::UNIQUE);
        connect(m_controller, &MusigAccountController::updateBalance, this,
                &MusigAccountAdapter::onControllerBalanceUpdate, qontrol::UNIQUE);
    }
}

auto MusigAccountAdapter::implemented() const -> bool {
    return true;
}

auto MusigAccountAdapter::name() -> ReqId {
    ReqId reqId = nextReqId();
    if (!accountReady(reqId)) {
        return reqId;
    }
    emit IAccount::name(reqId, toQString(m_controller->getAccount().value()->name()));
    return reqId;
}

auto MusigAccountAdapter::info() -> ReqId {
    ReqId reqId = nextReqId();
    if (!accountReady(reqId)) {
        return reqId;
    }
    QMap<QString, QString> out;
    out.insert(QStringLiteral("kind"), QStringLiteral("musig"));
    out.insert(QStringLiteral("displayName"),
               toQString(m_controller->getAccount().value()->name()));
    emit IAccount::info(reqId, out);
    return reqId;
}

auto MusigAccountAdapter::coins() -> ReqId {
    ReqId reqId = nextReqId();
    emitCoins(reqId);
    return reqId;
}

auto MusigAccountAdapter::balance() -> ReqId {
    ReqId reqId = nextReqId();
    emitBalance(reqId);
    return reqId;
}

auto MusigAccountAdapter::newReceiveAddress() -> ReqId {
    ReqId reqId = nextReqId();
    if (!accountReady(reqId)) {
        return reqId;
    }
    emit receiveAddress(reqId, m_controller->newAddress(AddressKind::Taproot));
    return reqId;
}

auto MusigAccountAdapter::newChangeAddress() -> ReqId {
    ReqId reqId = nextReqId();
    emit error(reqId, QStringLiteral("MuSig2 account has no change address request"));
    return reqId;
}

auto MusigAccountAdapter::descriptor() -> ReqId {
    ReqId reqId = nextReqId();
    if (!accountReady(reqId)) {
        return reqId;
    }
    emit IAccount::descriptor(reqId, toQString(m_controller->getDescriptor()));
    return reqId;
}

auto MusigAccountAdapter::raw(const QByteArray &request) -> ReqId {
    Q_UNUSED(request);
    ReqId reqId = nextReqId();
    emit error(reqId, QStringLiteral("MuSig2 account raw() not implemented"));
    emit IAccount::raw(reqId, QByteArray());
    return reqId;
}

void MusigAccountAdapter::onControllerCoinsUpdate() {
    emitCoins(0);
}

void MusigAccountAdapter::onControllerBalanceUpdate([[maybe_unused]] uint64_t balance) {
    emitBalance(0);
}

auto MusigAccountAdapter::accountReady(ReqId req_id) -> bool {
    if (m_controller == nullptr || !m_controller->getAccount().has_value()) {
        emit error(req_id, QStringLiteral("MuSig2 account not initialized"));
        return false;
    }
    return true;
}

auto MusigAccountAdapter::nextReqId() -> ReqId {
    ++m_next_req_id;
    if (m_next_req_id == 0) {
        ++m_next_req_id;
    }
    return m_next_req_id;
}

void MusigAccountAdapter::emitCoins(ReqId req_id) {
    if (m_controller == nullptr || !m_controller->getAccount().has_value()) {
        emit error(requestOf(req_id), QStringLiteral("MuSig2 account controller missing"));
        return;
    }
    QList<plugin::Coin> out;
    auto result = m_controller->getAccount().value()->spending_coins();
    if (!result.is_ok) {
        emit error(requestOf(req_id), toQString(result.error));
        return;
    }
    out.reserve(static_cast<qsizetype>(result.coins.size()));
    for (const auto &coin : result.coins) {
        out.append(account::spendingCoin(coin));
    }
    emit IAccount::coins(req_id, out);
}

void MusigAccountAdapter::emitBalance(ReqId req_id) {
    if (m_controller == nullptr || !m_controller->getAccount().has_value()) {
        emit error(requestOf(req_id), QStringLiteral("MuSig2 account not initialized"));
        return;
    }
    auto state = m_controller->getAccount().value()->spendable_coins();
    plugin::Balance out{};
    out.confirmed = state.confirmed_balance;
    out.unconfirmed = state.unconfirmed_balance;
    emit IAccount::balance(req_id, out);
}
