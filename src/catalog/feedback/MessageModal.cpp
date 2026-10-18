#include "catalog/feedback/MessageModal.h"

#include "catalog/feedback/ModalStatus.h"
#include "theme/Icon.h"

#include <Qontrol>

namespace catalog {

MessageModal::MessageModal(const QString &title, const QString &message, Tone tone) {
    auto *status = new ModalStatus;
    if (tone == Tone::Success) {
        // A success is one short status line, as the title.
        status->setIcon(icon::circleCheck, IconColor::Success);
        status->setTitle(message);
    } else {
        status->setIcon(icon::alert, IconColor::Error);
        status->setSubtitle(message);
    }
    setWindowTitle(title);
    setWidth(ModalWidth::XS);
    setBody(status);
}

} // namespace catalog
