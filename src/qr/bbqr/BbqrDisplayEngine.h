#pragma once

#include "qr/QrDisplayEngine.h"

namespace qr {

// Plays a payload as BBQR frames: each density level packs more bytes in a frame,
// so fewer, denser frames.
class BbqrDisplayEngine : public QrDisplayEngine {
    Q_OBJECT

public:
    explicit BbqrDisplayEngine(const QrDisplaySettings &settings, QObject *parent = nullptr);
};

} // namespace qr
