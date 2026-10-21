#pragma once

#include <interfaces/keys.h>

namespace smoke {

class KeysBoundaryProbe final : public QObject {
public:
    int answers = 0;
    ReqId request = 0;
    plugin::SpendError kind = plugin::SpendError::Other;

public slots:

    void onCoordinationError(ReqId req_id, plugin::SpendError error_kind,
                             [[maybe_unused]] const QString &message) {
        ++answers;
        request = req_id;
        kind = error_kind;
    }
};

} // namespace smoke
