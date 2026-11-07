#pragma once

#include "account/services/AccountServices.h"
#include "modules/musig/MusigAccountController.h"

#include <QByteArray>
#include <QString>
#include <silent.h>

namespace musig {

// A Multikey account's service context. A broadcast marks the saved transaction it
// belongs to, so it runs on the controller's storage queue, behind the saved progress
// writes.
class MusigServices final : public account::AccountServices<MusigAccountController, MusigAccount> {
public:
    MusigServices(const QString &account, MusigAccountController *controller);

    void broadcast(backend::RequestId token, QByteArray transaction) override;
};

} // namespace musig
