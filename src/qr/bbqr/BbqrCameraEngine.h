#pragma once

#include "qr/QrCameraEngine.h"

#include <silent.h>

namespace qr {

// Joins BBQR parts scanned off camera frames back into their payload.
class BbqrCameraEngine : public QrCameraEngine {
    Q_OBJECT

public:
    explicit BbqrCameraEngine(QObject *parent = nullptr);
    ~BbqrCameraEngine() override;

protected:
    auto scan(const QImage &image) -> QrScan override;
    void clear() override;

private:
    rust::Box<::BbqrScanner> m_scanner;
};

} // namespace qr
