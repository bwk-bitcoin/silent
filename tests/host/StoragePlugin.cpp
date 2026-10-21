#include "interfaces/module.h"
#include <QVariant>
#include <utility>

// An external plugin with two modules, so storage callers have a real library identity.
// A "claim" property set on the plugin or a module replaces the id its meta() reports,
// as a library changing its metadata after it was activated would.
class Q_DECL_HIDDEN StorageModule final : public IModule {
    Q_OBJECT

public:
    StorageModule(QString id, QObject *parent) : IModule(parent), m_id(std::move(id)) {
    }

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        QString id = property("claim").isValid() ? property("claim").toString() : m_id;
        return {{QStringLiteral("id"), id}, {QStringLiteral("name"), id}};
    }

    auto list() -> ReqId override {
        return ++m_next;
    }

    auto autoStart() -> ReqId override {
        return ++m_next;
    }

    void startInstance([[maybe_unused]] const QString &id) override {
    }

private:
    QString m_id;
    ReqId m_next = 0;
};

class Q_DECL_HIDDEN StoragePlugin final : public QObject, public IPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SILENT_IPlugin_IID)
    Q_INTERFACES(IPlugin)

public:
    StoragePlugin()
        : m_first(new StorageModule(QStringLiteral("storage.first"), this)),
          m_second(new StorageModule(QStringLiteral("storage.second"), this)) {
    }

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        QString id = property("claim").isValid() ? property("claim").toString()
                                                 : QStringLiteral("storage.fixture");
        return {{QStringLiteral("id"), id}};
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
        return {m_first, m_second};
    }

private:
    StorageModule *m_first = nullptr;
    StorageModule *m_second = nullptr;
};

#include "StoragePlugin.moc"
