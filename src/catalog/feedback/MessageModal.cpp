#include "catalog/feedback/MessageModal.h"

#include "catalog/feedback/ModalStatus.h"
#include "theme/Icon.h"

namespace catalog {

MessageModal::MessageModal(const QString &title, const QString &message, Tone tone) {
    auto *status = new ModalStatus;
    if (tone == Tone::Success) {
        status->setIcon(icon::circleCheck, IconColor::Success);
    } else {
        status->setIcon(icon::alert, IconColor::Error);
    }
    status->setSubtitle(message);
    showStep(addStep(title, ModalWidth::XS, status, nullptr));
}

} // namespace catalog
