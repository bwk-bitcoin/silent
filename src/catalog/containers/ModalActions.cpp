#include "catalog/containers/ModalActions.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"
#include "theme/Palette.h"

#include <QPushButton>
#include <Qontrol>
#include <common.h>

namespace catalog {

ModalActions::ModalActions(QWidget *parent) : QWidget(parent) {
    m_reason = new Label(LabelRole::Caption, this);
    m_reason->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_reason->setVisible(false);

    m_row = (new qontrol::Row)
                ->margins(0, resolve(Spacing::S), 0, 0)
                ->spacing(resolve(Spacing::S))
                ->pushSpacer()
                ->push(m_reason);
    m_row->into(this);
}

void ModalActions::setBack(Back back) {
    if (m_back != nullptr) {
        m_row->remove(m_back);
        m_back = nullptr;
    }
    switch (back) {
    case Back::None:
        return;
    case Back::Back:
        m_back = btnBack();
        break;
    case Back::Previous:
        m_back = btnPrevious();
        break;
    case Back::OtherSigner:
        m_back = btnOtherSigner();
        break;
    case Back::AllSettings:
        m_back = btnAllSettings();
        break;
    }
    m_row->insert(0, m_back);
    connect(m_back, &QPushButton::clicked, this, &ModalActions::backClicked, qontrol::UNIQUE);
}

void ModalActions::setReason(const QString &reason) {
    m_reason->setTextWrapped(reason, metric::ACTIONS_REASON_MAX_WIDTH);
    m_reason->setVisible(!reason.isEmpty());
}

void ModalActions::addAction(QPushButton *action) {
    m_row->push(action);
}

} // namespace catalog
