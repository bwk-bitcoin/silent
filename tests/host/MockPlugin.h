#pragma once

#include <QAtomicInt>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <host/Host.h>
#include <interfaces/account.h>
#include <interfaces/feed.h>
#include <interfaces/instance.h>
#include <interfaces/module.h>
#include <interfaces/signer.h>
#include <interfaces/theme.h>
#include <interfaces/types.h>

namespace smoke {

// Signer capability for the listing checks. Requests are inert (they return 0);
// only implemented() and the immutable accessors matter here.
class MockSigner final : public ISigner {
    Q_OBJECT

public:
    MockSigner(QString id, bool implemented, QString fingerprint, plugin::SignerState state,
               QObject *parent = nullptr)
        : ISigner(parent), m_id(std::move(id)), m_fingerprint(std::move(fingerprint)),
          m_state(state), m_implemented(implemented) {}
    ~MockSigner() override = default;

    [[nodiscard]] bool implemented() const override {
        return m_implemented;
    }

    [[nodiscard]] QString id() const override {
        return m_id;
    }

    [[nodiscard]] QString fingerprint() const override {
        return m_fingerprint;
    }

    [[nodiscard]] QString walletName() const override {
        return QString();
    }

    [[nodiscard]] plugin::SignerState state() const override {
        return m_state;
    }

    [[nodiscard]] QString stateDetail() const override {
        return m_state == plugin::SignerState::Ready ? QString() : QStringLiteral("mock detail");
    }

    auto init() -> ReqId override {
        return 0;
    }

    auto info() -> ReqId override {
        return 0;
    }

    auto getXpub(const QString &path) -> ReqId override {
        Q_UNUSED(path);
        return 0;
    }

    auto isDescriptorRegistered(const QString &descriptor) -> ReqId override {
        Q_UNUSED(descriptor);
        return 0;
    }

    auto registerDescriptor(const QString &descriptor) -> ReqId override {
        Q_UNUSED(descriptor);
        return 0;
    }

    auto signWithDescriptor(const QString &psbt_base64, const QString &descriptor)
        -> ReqId override {
        Q_UNUSED(psbt_base64);
        Q_UNUSED(descriptor);
        return 0;
    }

    auto raw(const QByteArray &request) -> ReqId override {
        Q_UNUSED(request);
        return 0;
    }

private:
    QString m_id;
    QString m_fingerprint;
    plugin::SignerState m_state = plugin::SignerState::Ready;
    bool m_implemented = false;
};

// Minimal IInstance used by the host smoke test. The account/feed/theme providers
// are never dereferenced, so they return nullptr by contract. signer() returns
// nullptr unless the instance was built with one, which covers the three shapes
// signers() has to handle: absent, inert, implemented.
class MockInstance final : public QObject, public IInstance {
    Q_OBJECT
    Q_INTERFACES(IInstance)

public:
    explicit MockInstance(QString id, QObject *parent = nullptr)
        : QObject(parent), m_id(std::move(id)) {}
    MockInstance(QString id, bool signer_implemented, QString fingerprint,
                 plugin::SignerState state, QObject *parent = nullptr)
        : QObject(parent), m_id(std::move(id)),
          m_signer(new MockSigner(m_id, signer_implemented, std::move(fingerprint), state, this)) {}
    ~MockInstance() override = default;

    [[nodiscard]] QString id() const override {
        return m_id;
    }

    void stop() override {
        m_stopped = true;
    }

    auto account() -> IAccount * override {
        return nullptr;
    }

    auto feed() -> IFeed * override {
        return nullptr;
    }

    auto signer() -> ISigner * override {
        return m_signer;
    }

    auto theme() -> IThemeProvider * override {
        return nullptr;
    }

    [[nodiscard]] auto wasStopped() const -> bool {
        return m_stopped;
    }

    // The instance-driven teardown path: an instance that notices its own device
    // is gone retires itself.
    void retire() {
        Host::get()->retireInstance(this);
    }

private:
    QString m_id;
    bool m_stopped = false;
    MockSigner *m_signer = nullptr;
};

// Mock module modelling the real async modules: list()/autoStart() return a
// request id without emitting, then flush() delivers the responses. This lets
// the registry register the pending request before the signal arrives, so the
// response is matched with the module as sender(). startInstance() registers a
// MockInstance via the host.
class MockModule final : public IModule {
    Q_OBJECT

public:
    explicit MockModule(QObject *parent = nullptr) : IModule(parent) {}
    ~MockModule() override = default;

    [[nodiscard]] QMap<QString, QString> meta() const override {
        QMap<QString, QString> meta;
        meta.insert(QStringLiteral("id"), QStringLiteral("mock.module"));
        meta.insert(QStringLiteral("name"), QStringLiteral("Mock Module"));
        return meta;
    }

    auto list() -> ReqId override {
        m_list_req_id = ++m_next_req_id;
        return m_list_req_id;
    }

    auto autoStart() -> ReqId override {
        m_auto_start_req_id = ++m_next_req_id;
        return m_auto_start_req_id;
    }

    // Deliver the pending list()/autoStart() responses. Emitted from the module
    // itself so the registry sees sender() == this module.
    void flush() {
        if (m_auto_start_req_id != 0) {
            ReqId req_id = m_auto_start_req_id;
            m_auto_start_req_id = 0;
            emit IModule::autoStart(req_id, QStringList{});
        }
        if (m_list_req_id != 0) {
            ReqId req_id = m_list_req_id;
            m_list_req_id = 0;
            QList<QPair<QString, QString>> instances;
            instances.append({QStringLiteral("inst-1"), QStringLiteral("Instance One")});
            emit IModule::instances(req_id, instances);
        }
    }

    void startInstance(const QString &id) override {
        auto *instance = new MockInstance(id.trimmed(), this);
        m_live_instances.append(instance);
        Host::get()->registerInstance(id.trimmed(), instance, this);
    }

    // Registers outside the host-driven startInstance path, the shape a signer
    // module uses when a device shows up.
    // Builds and tracks an instance without registering it, so a test can register
    // it from another thread without parenting a QObject across threads.
    auto createInstance(const QString &id, bool signer_implemented,
                        const QString &fingerprint = QString(),
                        plugin::SignerState state = plugin::SignerState::Ready) -> MockInstance * {
        auto *instance =
            new MockInstance(id.trimmed(), signer_implemented, fingerprint, state, this);
        m_live_instances.append(instance);
        return instance;
    }

    auto registerLive(const QString &id, bool signer_implemented,
                      const QString &fingerprint = QString(),
                      plugin::SignerState state = plugin::SignerState::Ready) -> MockInstance * {
        auto *instance = createInstance(id, signer_implemented, fingerprint, state);
        Host::get()->registerInstance(instance->id(), instance, this);
        return instance;
    }

    // The module-driven teardown path, the shape used when a device goes away.
    void dropLive(const QString &id) {
        Host::get()->removeInstance(id.trimmed(), this);
    }

    void deleteInstance(const QString &id) override {
        m_deleted_ids.append(id.trimmed());
    }

    // The host's side of the switch: every enabled module is told, whether or not it
    // asked for anything.
    void setSignerPolling(bool enabled) override {
        m_polling = enabled;
        ++m_polling_calls;
    }

    // This module's side: it asks the host, like any consumer would.
    void requestPolling(bool enabled) {
        Host::get()->requestSignerPolling(this, enabled);
    }

    [[nodiscard]] auto polling() const -> bool {
        return m_polling;
    }

    [[nodiscard]] auto pollingCalls() const -> int {
        return m_polling_calls;
    }

    [[nodiscard]] auto deletedIds() const -> QStringList {
        return m_deleted_ids;
    }

    [[nodiscard]] auto liveInstance(const QString &id) const -> MockInstance * {
        for (auto *instance : m_live_instances) {
            if (instance != nullptr && instance->id() == id.trimmed()) {
                return instance;
            }
        }
        return nullptr;
    }

private:
    ReqId m_next_req_id = 0;
    ReqId m_list_req_id = 0;
    ReqId m_auto_start_req_id = 0;
    QList<MockInstance *> m_live_instances;
    QStringList m_deleted_ids;
    bool m_polling = false;
    int m_polling_calls = 0;
};

// Drives the three host lifecycle calls from whatever thread it lives in. The
// instance itself is built on the GUI thread; only the host calls are moved.
class ThreadedCaller final : public QObject {
    Q_OBJECT

public:
    ThreadedCaller(MockModule *module, MockInstance *instance, QObject *parent = nullptr)
        : QObject(parent), m_module(module), m_instance(instance) {}
    ~ThreadedCaller() override = default;

    // Bumped once each call returns, so the GUI thread can tell "the worker has
    // not called yet" from "the worker called and the host has not run it yet"
    // without spinning its own event loop.
    [[nodiscard]] auto callsDone() const -> int {
        return m_calls_done.loadAcquire();
    }

public slots:
    void registerFromThread() {
        Host::get()->registerInstance(m_instance->id(), m_instance, m_module);
        m_calls_done.ref();
    }

    void dropFromThread() {
        Host::get()->removeInstance(m_instance->id(), m_module);
        m_calls_done.ref();
    }

    void retireFromThread() {
        m_instance->retire();
        m_calls_done.ref();
    }

private:
    MockModule *m_module = nullptr;
    MockInstance *m_instance = nullptr;
    QAtomicInt m_calls_done;
};

// Counts emissions of a signal via a named slot (no lambdas in connect()).
class SignalSpy final : public QObject {
    Q_OBJECT

public:
    explicit SignalSpy(QObject *parent = nullptr) : QObject(parent) {}
    ~SignalSpy() override = default;

    [[nodiscard]] auto count() const -> int {
        return m_count;
    }

public slots:
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void onSignal() {
        ++m_count;
    }

private:
    int m_count = 0;
};

// Builtin-style plugin exposing the single mock module.
class MockPlugin final : public QObject, public IPlugin {
    Q_OBJECT
    Q_INTERFACES(IPlugin)

public:
    explicit MockPlugin(QObject *parent = nullptr)
        : QObject(parent), m_module(new MockModule(this)),
          m_other_module(new MockModule(this)) {}
    ~MockPlugin() override = default;

    [[nodiscard]] QMap<QString, QString> meta() const override {
        QMap<QString, QString> meta;
        meta.insert(QStringLiteral("id"), QStringLiteral("mock.plugin"));
        meta.insert(QStringLiteral("name"), QStringLiteral("Mock Plugin"));
        return meta;
    }

    [[nodiscard]] QList<IModule *> modules() const override {
        return {m_module, m_other_module};
    }

    [[nodiscard]] auto module() const -> MockModule * {
        return m_module;
    }

    // Second module of the same plugin, used to check that a module cannot drop
    // an instance it does not own.
    [[nodiscard]] auto otherModule() const -> MockModule * {
        return m_other_module;
    }

private:
    MockModule *m_module = nullptr;
    MockModule *m_other_module = nullptr;
};

} // namespace smoke
