#include "PluginStateJob.h"

#include <common.h>
#include <string>
#include <utility>

PluginStateJob::PluginStateJob(PluginStateAction action, QString plugin_id, quint64 request,
                               quint64 revision, bool clear_errors)
    : m_action(action),
      m_plugin_id(std::move(plugin_id)),
      m_request(request),
      m_revision(revision),
      m_clear_errors(clear_errors) {
    connect(this, &QThread::finished, this, &PluginStateJob::onFinished, qontrol::UNIQUE);
}

void PluginStateJob::setReceiver(QObject *receiver) {
    m_receiver = receiver;
    connect(receiver, &QObject::destroyed, this, &PluginStateJob::onReceiverDestroyed,
            qontrol::UNIQUE);
}

auto PluginStateJob::action() const -> PluginStateAction {
    return m_action;
}

auto PluginStateJob::pluginId() const -> QString {
    return m_plugin_id;
}

auto PluginStateJob::request() const -> quint64 {
    return m_request;
}

auto PluginStateJob::revision() const -> quint64 {
    return m_revision;
}

auto PluginStateJob::clearErrors() const -> bool {
    return m_clear_errors;
}

auto PluginStateJob::error() const -> QString {
    return m_error;
}

auto PluginStateJob::enablePersisted() const -> bool {
    return m_enable_persisted;
}

auto PluginStateJob::state() const -> const PluginRemovalListResult & {
    return m_state;
}

auto PluginStateJob::metadata() const -> const rust::Vec<PluginMetadata> & {
    return m_metadata;
}

auto PluginStateJob::library() const -> const PluginLibraryResult & {
    return m_library;
}

void PluginStateJob::run() {
    auto id = rust::String(m_plugin_id.toStdString());
    rust::String error;
    switch (m_action) {
    case PluginStateAction::Scan:
        m_state = ::app_removing_plugins();
        if (m_state.is_ok) {
            m_metadata = ::plugin_metadata();
        } else {
            error = m_state.error;
        }
        break;
    case PluginStateAction::Enable:
        error = ::app_set_plugin_enabled(id, true);
        if (error.empty()) {
            m_enable_persisted = true;
            m_library = ::plugin_library(id);
            if (!m_library.is_ok) {
                error = m_library.error;
            }
        }
        break;
    case PluginStateAction::Disable:
        error = ::app_disable_plugin(id);
        break;
    case PluginStateAction::BeginRemoval:
        error = ::app_begin_plugin_removal(id);
        break;
    case PluginStateAction::RemoveData:
        error = ::plugin_remove(id);
        break;
    }
    m_error = QString::fromStdString(std::string(error));
}

void PluginStateJob::onFinished() {
    // A live receiver owns cleanup after consuming results, including nested event loops.
    if (m_receiver == nullptr) {
        deleteLater();
    }
}

void PluginStateJob::onReceiverDestroyed() {
    m_receiver = nullptr;
    if (isFinished()) {
        deleteLater();
    }
}
