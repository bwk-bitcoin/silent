#include "listing_reentrancy_smoke.h"

#include "account/AccountModule.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMap>
#include <QThread>
#include <common.h>
#include <silent.h>

namespace smoke {
namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("listing reentry: %s", message);
    }
}

class ListingModule final : public account::AccountModule {
public:
    ListingModule() : AccountModule("sp", ::sp_account_delete_check, ::delete_sp_account) {
    }

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {};
    }

    auto autoStart() -> ReqId override {
        return 0;
    }

    void startInstance([[maybe_unused]] const QString &id) override {
    }
};
} // namespace

ListingReentryProbe::ListingReentryProbe(account::AccountModule *module) : m_module(module) {
    connect(module, &IModule::instances, this, &ListingReentryProbe::onInstances, qontrol::UNIQUE);
}

void ListingReentryProbe::run() {
    m_requested.append(m_module->list());
    QElapsedTimer deadline;
    deadline.start();
    while (m_replied.size() < 3 && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(m_requested == QList<ReqId>{1, 2, 3}, "three distinct list requests are allocated");
    require(m_replied == QList<ReqId>{1, 2, 3},
            "reentrant requests each complete exactly once in original request order");
}

void ListingReentryProbe::onInstances(ReqId request,
                                      const QList<QPair<QString, QString>> &entries) {
    require(entries.isEmpty(), "the isolated fixture has no built-in account configs");
    m_replied.append(request);
    if (m_replied.size() == 1) {
        m_requested.append(m_module->list());
        m_requested.append(m_module->list());
    }
}

void listingReentrySmoke() {
    ListingModule module;
    ListingReentryProbe probe(&module);
    probe.run();
}
} // namespace smoke
