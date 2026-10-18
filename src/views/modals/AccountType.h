#pragma once

#include <QList>
#include <qevent.h>

#include "catalog/containers/Modal.h"
#include "host/PluginRegistry.h"

namespace catalog {
class SelectRow;
} // namespace catalog

namespace modal {

// The account types to create from the launcher: the built-in creators, then those of
// the enabled plugins. Picking one starts its creator and closes the modal.
class AccountType : public catalog::Modal {
    Q_OBJECT

public:
    explicit AccountType(const QList<PluginRegistry::CreatorLauncher> &creators);

public slots:
    void onCreatorClicked();

protected:
    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    QList<PluginRegistry::CreatorLauncher> m_creators;
    QList<catalog::SelectRow *> m_rows;
};

} // namespace modal
