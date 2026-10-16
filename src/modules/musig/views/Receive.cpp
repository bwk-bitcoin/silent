#include "modules/musig/views/Receive.h"

namespace musig::view {

Receive::Receive(MusigAccountController *ctrl) {
    Q_UNUSED(ctrl);
}

void Receive::init() {
}

void Receive::doConnect() {
}

void Receive::view() {
}

// ===== Slots =====

void Receive::onNewAddressClicked() {
}

void Receive::onVerifyAddressClicked() {
}

void Receive::onAddressVerified(ReqId req_id, bool verified) {
    Q_UNUSED(req_id);
    Q_UNUSED(verified);
}

void Receive::onKeysError(std::optional<ReqId> req_id, const QString &message,
                          plugin::SignError kind) {
    Q_UNUSED(req_id);
    Q_UNUSED(message);
    Q_UNUSED(kind);
}

} // namespace musig::view
