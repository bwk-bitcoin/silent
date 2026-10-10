#pragma once

#include "catalog/containers/Modal.h"
#include <QString>

namespace catalog {

// A modal telling the outcome of an action: an icon and the message, closed by the
// window's own close (no button, design rule).
class MessageModal : public Modal {
    Q_OBJECT

public:
    enum class Tone { Error, Success };

    MessageModal(const QString &title, const QString &message, Tone tone = Tone::Error);
};

} // namespace catalog
