#include "modules/musig/MusigAccountAdapter.h"

MusigAccountAdapter::MusigAccountAdapter(MusigAccountController *controller) {
    Q_UNUSED(controller);
}

auto MusigAccountAdapter::implemented() const -> bool {
    return false;
}

auto MusigAccountAdapter::name() -> ReqId {
    return 0;
}

auto MusigAccountAdapter::info() -> ReqId {
    return 0;
}

auto MusigAccountAdapter::coins() -> ReqId {
    return 0;
}

auto MusigAccountAdapter::balance() -> ReqId {
    return 0;
}

auto MusigAccountAdapter::newReceiveAddress() -> ReqId {
    return 0;
}

auto MusigAccountAdapter::newChangeAddress() -> ReqId {
    return 0;
}

auto MusigAccountAdapter::raw(const QByteArray &request) -> ReqId {
    Q_UNUSED(request);
    return 0;
}

void MusigAccountAdapter::onControllerCoinsUpdate() {
}

void MusigAccountAdapter::onControllerBalanceUpdate(uint64_t balance) {
    Q_UNUSED(balance);
}

auto MusigAccountAdapter::accountReady(ReqId req_id) -> bool {
    Q_UNUSED(req_id);
    return false;
}

auto MusigAccountAdapter::nextReqId() -> ReqId {
    return 0;
}

void MusigAccountAdapter::emitCoins(ReqId req_id) {
    Q_UNUSED(req_id);
}

void MusigAccountAdapter::emitBalance(ReqId req_id) {
    Q_UNUSED(req_id);
}
