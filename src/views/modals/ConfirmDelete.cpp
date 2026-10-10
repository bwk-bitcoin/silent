#include "ConfirmDelete.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "i18n/Tr.h"

#include <Qontrol>
#include <common.h>

namespace modal {

ConfirmDelete::ConfirmDelete(const QString &name) : m_name(name) {
    init();
    doConnect();
    view();
}

void ConfirmDelete::init() {
    m_label = new catalog::Label;
    m_label->setWordWrap(true);
    m_delete_btn = catalog::btnDelete();
}

void ConfirmDelete::doConnect() {
    connect(m_delete_btn, &QPushButton::clicked, this, &ConfirmDelete::onDeleteClicked,
            qontrol::UNIQUE);
}

void ConfirmDelete::onDeleteClicked() {
    emit confirmed(m_name);
    accept();
}

void ConfirmDelete::view() {
    auto *actions = new catalog::ModalActions;
    actions->addAction(m_delete_btn);
    setBody(m_label);
    setFooter(actions);
    setWidth(ModalWidth::XXS);
    retranslateUi();
}

void ConfirmDelete::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void ConfirmDelete::retranslateUi() {
    m_label->setText(TR("delete-wallet-confirmation").arg(m_name));
    m_delete_btn->setText(TR("common-delete"));
    setWindowTitle(TR("delete-wallet-title"));
    adjustSize();
}

} // namespace modal
