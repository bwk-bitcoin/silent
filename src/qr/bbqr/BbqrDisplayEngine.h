#pragma once

#include "qr/QrDisplayEngine.h"

namespace qr {

// Plays a payload as BBQR frames, each carrying the density's bytes.
class BbqrDisplayEngine : public QrDisplayEngine {
    Q_OBJECT

public:
    explicit BbqrDisplayEngine(QObject *parent = nullptr);
    ~BbqrDisplayEngine() override;

protected:
    [[nodiscard]] auto render(const QByteArray &payload, QrDensity density) const
        -> QrRender override;
};

} // namespace qr
