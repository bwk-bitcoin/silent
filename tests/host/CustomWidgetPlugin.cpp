#include "interfaces/host.h"
#include "interfaces/instance.h"
#include "interfaces/module.h"

#include <QPointer>
#include <QPushButton>

class Q_DECL_HIDDEN CustomInstance final : public QObject, public IInstance {
    Q_OBJECT
    Q_INTERFACES(IInstance)

public:
    explicit CustomInstance(QObject *parent) : QObject(parent) {
    }

    ~CustomInstance() override {
        delete m_widget;
    }

    [[nodiscard]] auto id() const -> QString override {
        return QStringLiteral("custom-widget-instance");
    }

    void stop() override {
        // The host removes the page after stop(); destruction drains its deferred deletion.
    }

    auto account() -> IAccount * override {
        return nullptr;
    }

    auto feed() -> IFeed * override {
        return nullptr;
    }

    auto signer() -> ISigner * override {
        return nullptr;
    }

    auto theme() -> IThemeProvider * override {
        return nullptr;
    }

    void publish() {
        m_widget = new QPushButton(QStringLiteral("Independent plugin UI"));
        m_widget->setObjectName(QStringLiteral("custom-plugin-widget"));
        m_widget->setCheckable(true);
        auto tab = Host::get()->openTab(m_widget, this);
        if (tab == 0) {
            qFatal("custom widget publication failed");
        }
        Host::get()->setTabTitle(tab, QStringLiteral("Custom widget"));
    }

private:
    QPointer<QPushButton> m_widget;
};

class Q_DECL_HIDDEN CustomModule final : public IModule {
    Q_OBJECT

public:
    using IModule::IModule;

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("custom.widget.module")}};
    }

    auto list() -> ReqId override {
        emit instances(1, {{QStringLiteral("custom-widget-instance"), QStringLiteral("Custom")}});
        return 1;
    }

    auto autoStart() -> ReqId override {
        emit IModule::autoStart(2, {});
        return 2;
    }

    void startInstance([[maybe_unused]] const QString &id) override {
        auto *instance = new CustomInstance(this);
        Host::get()->registerInstance(instance->id(), instance, this);
        instance->publish();
    }
};

class Q_DECL_HIDDEN CustomWidgetPlugin final : public QObject, public IPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SILENT_IPlugin_IID)
    Q_INTERFACES(IPlugin)

public:
    CustomWidgetPlugin() : m_module(new CustomModule(this)) {
    }

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("custom.widget.fixture")}};
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
        return {m_module};
    }

private:
    CustomModule *m_module = nullptr;
};

#include "CustomWidgetPlugin.moc"
