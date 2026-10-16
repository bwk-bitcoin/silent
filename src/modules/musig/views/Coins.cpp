#include "modules/musig/views/Coins.h"

namespace musig::view {

Coins::Coins(MusigAccountController *ctrl) {
    Q_UNUSED(ctrl);
}

void Coins::init() {
}

void Coins::doConnect() {
}

void Coins::view() {
}

// ===== Slots =====

void Coins::onCoinsUpdated(CoinState state) {
    Q_UNUSED(state);
}

void Coins::onLabelEdited(const QString &outpoint, const QString &label) {
    Q_UNUSED(outpoint);
    Q_UNUSED(label);
}

} // namespace musig::view
