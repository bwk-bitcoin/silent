#pragma once

#include <QString>
#include <qevent.h>

#include "catalog/containers/Modal.h"

namespace catalog {
class Button;
class Label;
} // namespace catalog

namespace modal {

class ConfirmDelete : public catalog::Modal {
    Q_OBJECT

public:
    explicit ConfirmDelete(const QString &name);

signals:
    void confirmed(const QString &name);

public slots:
    void onDeleteClicked();

protected:
    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    QString m_name;
    catalog::Label *m_label = nullptr;
    catalog::Button *m_delete_btn = nullptr;
};

} // namespace modal
