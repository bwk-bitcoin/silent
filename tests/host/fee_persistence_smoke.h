#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtGlobal>

class HostImpl;

namespace smoke {

struct FeeWriteOutcome {
    quint8 mode;
    QString source;
    QString error;
};

class FeeWriteProbe final : public QObject {
    Q_OBJECT

public:
    explicit FeeWriteProbe(HostImpl *host);
    [[nodiscard]] auto outcomes() const -> const QList<FeeWriteOutcome> &;
    [[nodiscard]] auto ticks() const -> int;

public slots:
    void onFeePreferencesWritten(quint8 mode, const QString &source, const QString &error);
    void onTick();

private:
    QList<FeeWriteOutcome> m_outcomes;
    QTimer m_timer;
    int m_ticks = 0;
};

class FeeReentrantSelection final : public QObject {
    Q_OBJECT

public:
    explicit FeeReentrantSelection(HostImpl *host);

public slots:
    void onSelectedFeesChanged();

private:
    HostImpl *m_host = nullptr;
    bool m_reentered = false;
};

class FeeQuitDriver final : public QObject {
    Q_OBJECT

public:
    explicit FeeQuitDriver(HostImpl *host);

public slots:
    void onQuitRequested();

private:
    HostImpl *m_host = nullptr;
};

void feePersistenceSmoke();

} // namespace smoke
