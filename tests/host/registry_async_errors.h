#pragma once

#include <QObject>
#include <QPointer>

class PluginRegistry;
class PluginStateJob;

namespace smoke {
class RegistryJobLifetimeProbe final : public QObject {
    Q_OBJECT

public:
    explicit RegistryJobLifetimeProbe(PluginRegistry *registry);
    void arm();

    [[nodiscard]] auto observed() const -> bool {
        return m_observed;
    }

public slots:
    void onRegistryChanged();
    void onWorkerFinished();

private:
    QPointer<PluginRegistry> m_registry;
    QPointer<PluginStateJob> m_job;
    bool m_armed = false;
    bool m_observed = false;
};

void registryCheckpointFailureSmoke();
void registryEnableRollbackFailureSmoke();
void registryEnableRollbackReadErrorSmoke();
void registryStaleScanErrorsSmoke();
void registryAccountDeletionRefreshSmoke();
} // namespace smoke
