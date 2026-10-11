#pragma once

#include <cstdint>

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QList>
#include <QMetaType>
#include <QString>

namespace qr {

// Every tuning setting runs over the same levels.
constexpr int MIN_LEVEL = 1;
constexpr int MAX_LEVEL = 5;
constexpr int DEFAULT_LEVEL = 3;

// A request as a codec encoded it, for a transport to frame.
struct QrRequest {
    QByteArray payload;
};

// A payload a transport joined back from scanned frames, for a codec to decode.
struct QrResponse {
    QByteArray payload;
};

// One image of an animated request. A null image while the frames are rendered.
struct QrFrame {
    QImage image;
    // The quiet zone around the code, in the contrast the image was mapped to.
    QColor background;
    int index = 0;
    int count = 0;
    // Why the request could not be framed, empty otherwise.
    QString error;
};

// One bare camera picture, RGB888.
struct CameraFrame {
    QImage image;
    quint64 seq = 0;
};

// What a transport's framing of one payload gave.
struct QrRender {
    // Grayscale, 0 dark and 255 light.
    QList<QImage> frames;
    QString error;
};

// What one camera frame gave a transport's scan.
struct QrScan {
    QList<QByteArray> payloads;
    int seen = 0;
    int total = 0;
};

// At the lowest contrast the quiet zone is a dark grey, at the highest white.
struct QrDisplaySettings {
    int density = DEFAULT_LEVEL;
    int contrast = DEFAULT_LEVEL;
    int speed = DEFAULT_LEVEL;
};

// Contrast is applied to each received camera frame, before it is scanned.
struct QrCameraSettings {
    int contrast = DEFAULT_LEVEL;
};

// Why a codec could not start an exchange or refused an answer. Only RequestFailed
// ends the exchange: after the others the scan goes on.
enum class QrError : std::uint8_t { RequestFailed, WrongRequest, Unsupported, Refused };

} // namespace qr

Q_DECLARE_METATYPE(qr::QrRequest)
Q_DECLARE_METATYPE(qr::QrResponse)
Q_DECLARE_METATYPE(qr::QrFrame)
Q_DECLARE_METATYPE(qr::CameraFrame)
Q_DECLARE_METATYPE(qr::QrRender)
Q_DECLARE_METATYPE(qr::QrScan)
Q_DECLARE_METATYPE(qr::QrError)
