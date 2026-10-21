#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
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

public slots:
    void onInstances(ReqId req_id, const ExampleInstances &instances);

private:
    QPointer<IModule> m_module;
    QList<ExampleInstances> m_answers;
};

// Drives the real external example plugin built in `directory` through a legacy state
// migration and its asynchronous storage. HOME holds the unlocked app state and the
// example plugin's legacy blob, seeded before the run.
void exampleStorageSmoke(const QString &directory);

} // namespace smoke
