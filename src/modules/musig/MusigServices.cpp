#include "modules/musig/MusigServices.h"

#include "account/services/Conversions.h"

#include <memory>

namespace musig {

MusigServices::MusigServices(const QString &account, MusigAccountController *controller)
    : AccountServices(account, controller) {
}

void MusigServices::broadcast(backend::RequestId token, QByteArray transaction) {
    auto *controller = owner();
    auto *wallet = rustAccount();
    // The storage queue drops work once the controller stops.
    if (wallet == nullptr || !available() || controller->m_stopping ||
        controller->m_stop_requested) {
        emit failed(token, busy());
        return;
    }
    auto result = std::make_shared<backend::BroadcastResult>();
    controller->enqueueStorage(
        [wallet, result, transaction] {
            *result = account::services::toBroadcastResult(
                wallet->broadcast_report(account::services::toSlice(transaction)));
        },
        [this, token, result] { onBroadcastDone(token, *result); });
}

} // namespace musig
