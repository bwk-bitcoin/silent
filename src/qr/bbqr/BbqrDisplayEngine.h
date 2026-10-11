#pragma once

#include "qr/QrDisplayEngine.h"

namespace qr {

// Plays a payload as BBQR frames: each density level packs more bytes in a frame,
// so fewer, denser frames.
class BbqrDisplayEngine : public QrDisplayEngine {
    Q_OBJECT

public:
    explicit BbqrDisplayEngine(const QrDisplaySettings &settings, QObject *parent = nullptr);
    ~BbqrDisplayEngine() override;

protected:
    [[nodiscard]] auto render(const QByteArray &payload, int density) const -> QrRender override;
};

} // namespace qr
