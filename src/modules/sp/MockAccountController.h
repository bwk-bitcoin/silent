#pragma once

#include "AccountController.h"

// Debug-only controller: feeds the views consistent dummy data with no FFI
// account, so the panels can be exercised offline. Toggled in AccountWidget.
class MockAccountController : public AccountController {
public:
    explicit MockAccountController(AccountWidget *widget);

    auto loadPanels() -> void override;
    auto getCoins() -> rust::Vec<RustCoin> override;
    auto getPaymentHistory() -> rust::Vec<RustTx> override;
    auto getSpAddress() -> rust::String override;
    auto newSegwitAddr() -> rust::String override;
    auto newTaprootAddr() -> rust::String override;
    auto hasSegwit() -> bool override;
    auto hasTaproot() -> bool override;
    auto simulateTx(TransactionTemplate tx) -> TransactionSimulation override;

private:
    int m_segwit_index = 0;
    int m_taproot_index = 0;
};
