#pragma once

#include <QPointer>
#include <QString>
#include <QThread>
#include <silent.h>

enum class PluginStateAction { Scan, Enable, Disable, BeginRemoval, RemoveData };

// Only copied host values cross this boundary; no module, loader or registry is used by run().
class PluginStateJob final : public QThread {
    Q_OBJECT

public:
    PluginStateJob(PluginStateAction action, QString plugin_id, quint64 request, quint64 revision,
                   bool clear_errors);
    void setReceiver(QObject *receiver);
    [[nodiscard]] auto action() const -> PluginStateAction;
    [[nodiscard]] auto pluginId() const -> QString;
    [[nodiscard]] auto request() const -> quint64;
    [[nodiscard]] auto revision() const -> quint64;
    [[nodiscard]] auto clearErrors() const -> bool;
    [[nodiscard]] auto error() const -> QString;
    [[nodiscard]] auto enablePersisted() const -> bool;
    [[nodiscard]] auto state() const -> const PluginRemovalListResult &;
    [[nodiscard]] auto metadata() const -> const rust::Vec<PluginMetadata> &;
    [[nodiscard]] auto library() const -> const PluginLibraryResult &;

public slots:
    void onFinished();
    void onReceiverDestroyed();

protected:
    void run() override;

private:
    QPointer<QObject> m_receiver;
    PluginStateAction m_action;
    QString m_plugin_id;
    quint64 m_request;
    quint64 m_revision;
    bool m_clear_errors;
    QString m_error;
    bool m_enable_persisted = false;
    PluginRemovalListResult m_state{};
    rust::Vec<PluginMetadata> m_metadata;
    PluginLibraryResult m_library{};
};
