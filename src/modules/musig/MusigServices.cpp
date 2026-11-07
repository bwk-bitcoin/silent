#include "modules/musig/MusigServices.h"

namespace musig {

MusigServices::MusigServices(const QString &account, MusigAccountController *controller)
    : AccountServices(account, controller) {
}

void MusigServices::broadcast(backend::RequestId token, QByteArray transaction) {
    if (!available() || !owner()->startBroadcast(transaction, [this, token](const auto &result) {
            onBroadcastDone(token, result);
        })) {
        emit failed(token, busy());
    }
}

} // namespace musig
