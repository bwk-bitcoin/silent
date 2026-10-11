#pragma once

#include "qr/QrCameraEngine.h"

namespace qr {

// Joins BBQR parts scanned off camera frames back into their payload.
class BbqrCameraEngine : public QrCameraEngine {
    Q_OBJECT

public:
    explicit BbqrCameraEngine(const QrCameraSettings &settings, QObject *parent = nullptr);
};

} // namespace qr
