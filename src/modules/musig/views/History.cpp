#include "modules/musig/views/History.h"

namespace musig::view {

History::History(MusigAccountController *ctrl) {
    Q_UNUSED(ctrl);
}

void History::init() {
}

void History::doConnect() {
}

void History::view() {
}

// ===== Slots =====

void History::onCoinsUpdated(CoinState state) {
    Q_UNUSED(state);
}

} // namespace musig::view
