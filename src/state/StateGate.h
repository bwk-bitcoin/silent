#pragma once

#include <QObject>
#include <QString>

class QThread;

namespace keys::modal {
class Unseal;
}

namespace state {

// Opens the app state before anything reads it (ws-plaude/silent#37): stops on a
// state found in clear, creates it on the first start, unlocks it with its device or
// its password otherwise. False when the app must quit.
auto unlockAtStartup() -> bool;

// Unlocks with the password typed in `unseal`, on a worker: Argon2id takes a second.
class PasswordUnlock final : public QObject {
    Q_OBJECT

public:
    explicit PasswordUnlock(keys::modal::Unseal *unseal);
    // Waits for a running worker: it answers to this object.
    ~PasswordUnlock() override;

public slots:
    void onPasswordEntered(const QString &password);
    void onUnlocked(const QString &error);

private:
    keys::modal::Unseal *m_unseal = nullptr;
    QThread *m_worker = nullptr;
};

} // namespace state
