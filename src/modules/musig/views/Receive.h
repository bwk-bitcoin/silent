#pragma once

#include "interfaces/types.h"

#include <QString>
#include <Qontrol>
#include <optional>

class MusigAccountController;

namespace musig::view {

class Receive : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Receive(MusigAccountController *ctrl);

public slots:
    void onNewAddressClicked();
    void onVerifyAddressClicked();
    void onAddressVerified(ReqId req_id, bool verified);
    void onKeysError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
};

} // namespace musig::view
