#include "catalog/feedback/MessageModal.h"

#include "catalog/CopyButton.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/panels/receive/ChunkedAddress.h"
#include "theme/Icon.h"

#include <Qontrol>

namespace catalog {

MessageModal::MessageModal(const QString &title, const QString &message, Tone tone,
                           const QString &value) {
    auto *status = new ModalStatus;
    if (tone == Tone::Success) {
        status->setIcon(icon::circleCheck, IconColor::Success);
    } else {
        status->setIcon(icon::alert, IconColor::Error);
    }
    status->setSubtitle(message);
    setWindowTitle(title);
    setWidth(ModalWidth::XS);
    if (value.isEmpty()) {
        setBody(status);
        return;
    }
    // A long value has no break opportunity: the subtitle would cut it on both sides.
    auto *chunked = new ChunkedAddress;
    chunked->setWordWrap(true); // the unbroken string breaks to fit too
    chunked->setAddress(value);
    auto *copy = btnCopyValue();
    copy->setTextToCopy(value);
    auto *row = (new qontrol::Row)
                    ->spacing(resolve(Spacing::XS))
                    ->push(chunked)
                    ->pushAligned(copy, Qt::AlignVCenter);
    auto *card = new Card(Card::Role::Inset);
    card->setPad(Card::Pad::M);
    card->setContent(row);
    auto *column = (new qontrol::Column)->spacing(resolve(Spacing::M))->push(status)->push(card);
    setWideBody(column);
}

} // namespace catalog
