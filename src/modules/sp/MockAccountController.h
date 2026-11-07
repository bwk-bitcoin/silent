#pragma once

#include "AccountController.h"

namespace account {
class ServiceContext;
} // namespace account

// Debug-only controller: feeds the views consistent dummy data with no FFI account,
// through services of its own, so the panels can be exercised offline. Toggled in
// AccountWidget.
class MockAccountController : public AccountController {
public:
    explicit MockAccountController(AccountWidget *widget);

    auto loadPanels() -> void override;
    auto network() -> Network override;
    auto getDescriptor() -> rust::String override;
    auto hasSegwit() -> bool override;
    auto hasTaproot() -> bool override;

private:
    account::ServiceContext *m_mock_services = nullptr;
};
