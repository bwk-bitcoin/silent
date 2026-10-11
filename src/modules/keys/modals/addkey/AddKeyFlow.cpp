#include "AddKeyFlow.h"

#include "catalog/containers/ViewSlots.h"

#include <Qontrol>

namespace keys::modal {

AddKeyFlow::AddKeyFlow(QWidget *parent) : QWidget(parent) {
    m_slots = new catalog::ViewSlots;
    (new qontrol::Column)->push(m_slots)->into(this);
}

auto AddKeyFlow::viewSlots() const -> catalog::ViewSlots * {
    return m_slots;
}

} // namespace keys::modal
