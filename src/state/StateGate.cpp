#include "StateGate.h"

#include "DeviceUnlock.h"
#include "StateSetup.h"
#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/feedback/MessageModal.h"
#include "i18n/Tr.h"
#include "modules/keys/modals/Unseal.h"

#include <QDialog>
#include <QDir>
#include <QMetaObject>
#include <QThread>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <silent.h>
#include <string>

namespace state {

namespace {
auto toQt(const rust::String &value) -> QString {
    return QString::fromStdString(std::string(value));
}

void refuse(const QString &title, const QString &text) {
    catalog::MessageModal modal(title, text);
    modal.exec();
}
} // namespace

PasswordUnlock::PasswordUnlock(keys::modal::Unseal *unseal) : m_unseal(unseal) {
    connect(m_unseal, &keys::modal::Unseal::passwordEntered, this,
            &PasswordUnlock::onPasswordEntered, qontrol::UNIQUE);
}

PasswordUnlock::~PasswordUnlock() {
    if (m_worker != nullptr) {
        m_worker->wait();
        delete m_worker;
    }
}

void PasswordUnlock::onPasswordEntered(const QString &password) {
    if (m_worker != nullptr) {
        m_worker->wait();
        delete m_worker;
    }
    m_worker = QThread::create([this, typed = password.toStdString()]() mutable {
        QString error = toQt(::state_unlock_with_password(rust::String(typed)));
        std::ranges::fill(typed, '\0');
        QMetaObject::invokeMethod(this, "onUnlocked", Qt::QueuedConnection, Q_ARG(QString, error));
    });
    m_worker->start();
}

void PasswordUnlock::onUnlocked(const QString &error) {
    if (error.isEmpty()) {
        m_unseal->finish(true);
    } else {
        m_unseal->retry(TR("flow-password-wrong"));
    }
}

auto unlockAtStartup() -> bool {
    QString clear = toQt(::state_clear_file());
    if (QString home = QDir::homePath(); clear.startsWith(home + QLatin1Char('/'))) {
        clear = QLatin1Char('~') + clear.mid(home.size());
    }
    if (!clear.isEmpty()) {
        refuse(TR("state-clear-title"), TR("state-clear-text").arg(clear));
        return false;
    }
    auto info = ::state_unlock_info();
    if (!info.error.empty()) {
        refuse(TR("state-unreadable-title"), toQt(info.error));
        return false;
    }
    if (!info.exists) {
        StateSetup setup;
        return setup.exec() == QDialog::Accepted;
    }
    if (info.device) {
        DeviceUnlock unlock(info);
        return unlock.exec() == QDialog::Accepted;
    }
    keys::modal::Unseal unseal(TR("state-unlock-title"), TR("state-unlock-password-text"),
                               catalog::btnUnlock());
    unseal.setBackShown(false);
    unseal.setBusy(TR("state-unlocking"), TR("state-unlocking-sub"));
    PasswordUnlock unlock(&unseal);
    return unseal.exec() == QDialog::Accepted;
}

} // namespace state
