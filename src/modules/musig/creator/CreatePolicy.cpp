#include "modules/musig/creator/CreatePolicy.h"

namespace musig::modal {

CreatePolicy::CreatePolicy() {
}

void CreatePolicy::init() {
}

void CreatePolicy::doConnect() {
}

void CreatePolicy::view() {
}

void CreatePolicy::showStep(Step step) {
    Q_UNUSED(step);
}

auto CreatePolicy::network() const -> Network {
    return Network::Regtest;
}

void CreatePolicy::previewPolicy() {
}

void CreatePolicy::fetchNextXpub() {
}

void CreatePolicy::compilePolicy() {
}

void CreatePolicy::showReview() {
}

void CreatePolicy::registerNext() {
}

void CreatePolicy::verifyAddress() {
}

void CreatePolicy::install() {
}

// ===== Slots =====

void CreatePolicy::onNetworkChanged(int index) {
    Q_UNUSED(index);
}

void CreatePolicy::onNameEdited(const QString &text) {
    Q_UNUSED(text);
}

void CreatePolicy::onPolicyEdited() {
}

void CreatePolicy::onNextClicked() {
}

void CreatePolicy::onBackClicked() {
}

void CreatePolicy::onXpubReady(ReqId req_id, const QString &xpub, const QString &fingerprint,
                               const QString &path) {
    Q_UNUSED(req_id);
    Q_UNUSED(xpub);
    Q_UNUSED(fingerprint);
    Q_UNUSED(path);
}

void CreatePolicy::onDescriptorRegistered(ReqId req_id, const QString &fingerprint) {
    Q_UNUSED(req_id);
    Q_UNUSED(fingerprint);
}

void CreatePolicy::onAddressVerified(ReqId req_id, bool verified) {
    Q_UNUSED(req_id);
    Q_UNUSED(verified);
}

void CreatePolicy::onKeysError(std::optional<ReqId> req_id, const QString &message,
                               plugin::SignError kind) {
    Q_UNUSED(req_id);
    Q_UNUSED(message);
    Q_UNUSED(kind);
}

void CreatePolicy::onKeysCancelled(ReqId req_id) {
    Q_UNUSED(req_id);
}

} // namespace musig::modal
