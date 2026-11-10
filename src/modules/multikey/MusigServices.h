#pragma once

#include "account/services/AccountServices.h"
#include "modules/multikey/MusigAccountController.h"

#include <QByteArray>
#include <QString>
#include <silent.h>

namespace musig {

// A Multikey account's service context. A broadcast marks the saved transaction it
// belongs to and ends the signing session it finalized, so the controller runs it
// (MusigAccountController::startBroadcast()), refusing it as its own broadcast was.
class MusigServices final : public account::AccountServices<MusigAccountController, MusigAccount> {
public:
    MusigServices(const QString &account, MusigAccountController *controller);

    void broadcast(backend::RequestId token, QByteArray transaction) override;
};

} // namespace musig
