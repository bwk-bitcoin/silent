#pragma once

#include "qr/QrCameraEngine.h"

#include <silent.h>

namespace qr {

// Joins BBQR parts scanned off camera frames back into their payload; reading plain
// codes too, each one's text is a payload of its own.
class BbqrCameraEngine : public QrCameraEngine {
    Q_OBJECT

public:
    explicit BbqrCameraEngine(QObject *parent = nullptr, PlainCodes plain = PlainCodes::Ignore);
    ~BbqrCameraEngine() override;

protected:
    auto scan(const QImage &image) -> QrScan override;
    void clear() override;

private:
    rust::Box<::BbqrScanner> m_scanner;
};

} // namespace qr
