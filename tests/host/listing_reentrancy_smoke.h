#pragma once

#include "interfaces/types.h"

#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

namespace account {
class AccountModule;
}

namespace smoke {
class ListingReentryProbe final : public QObject {
    Q_OBJECT

public:
    explicit ListingReentryProbe(account::AccountModule *module);
    void run();

public slots:
    void onInstances(ReqId request, const QList<QPair<QString, QString>> &entries);

private:
    account::AccountModule *m_module;
    QList<ReqId> m_requested;
    QList<ReqId> m_replied;
};

void listingReentrySmoke();
} // namespace smoke
