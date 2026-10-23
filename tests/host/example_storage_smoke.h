#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <interfaces/module.h>
#include <interfaces/types.h>

namespace smoke {

using ExampleInstances = QList<QPair<QString, QString>>;

// Records every list() answer of the real example module, in order.
class ExampleInstancesProbe final : public QObject {
    Q_OBJECT

public:
    // Follows `module` from now on and forgets the answers of the module before.
    void attach(IModule *module);
    [[nodiscard]] auto count() const -> qsizetype;
    // The answer at `index`, waiting for it.
    auto waitForAnswer(qsizetype index) -> ExampleInstances;
    // The index of the first answer from `after` on that lists `id`, or that does not
    // when `listed` is false, waiting for it.
    auto waitForListing(const QString &id, bool listed, qsizetype after) -> qsizetype;
    [[nodiscard]] auto deletionCount() const -> qsizetype;
    auto waitForDeletion(qsizetype index) -> QString;

public slots:
    void onInstances(ReqId req_id, const ExampleInstances &instances);
    void onAccountDeleted(ReqId req_id, const QString &id, const QString &error);

private:
    QPointer<IModule> m_module;
    QList<ExampleInstances> m_answers;
    QStringList m_deletion_errors;
};

// Imports the real external example plugin ZIP at `bundle` and drives a legacy state
// migration and its asynchronous storage. HOME holds the unlocked app state and the
// example plugin's legacy blob, seeded before the run.
void exampleStorageSmoke(const QString &bundle);
void exampleReinstallSmoke(const QString &bundle);

} // namespace smoke
