#pragma once

#include <cstdint>

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QList>
#include <QMetaType>
#include <QString>

namespace qr {

// Each setting's value is what it means to the engine, not a level: levels are the
// steppers' abstraction, mapped by the ...FromLevel() functions below.

// Bytes each frame of an animated request carries: denser frames are fewer.
enum class QrDensity : std::uint32_t {
    Lowest = 50,
    Low = 100,
    Medium = 150,
    High = 200,
    Highest = 250
};

// Milliseconds each frame of an animated request stays on screen.
enum class QrSpeed : std::uint16_t {
    Slowest = 440,
    Slow = 360,
    Medium = 280,
    Fast = 200,
    Fastest = 120
};

// Lightness percent of the quiet zone around a displayed code: a dark grey at the
// lowest, white at the highest.
enum class QrContrast : std::uint8_t {
    Lowest = 40,
    Low = 55,
    Medium = 70,
    High = 85,
    Highest = 100
};

// Contrast factor applied to each camera frame before it is scanned, in percent.
enum class QrCameraContrast : std::uint8_t {
    Lowest = 83,
    Low = 111,
    Medium = 139,
    High = 167,
    Highest = 195
};

// The steppers' levels, lowest to highest; DEFAULT_LEVEL is each setting's Medium.
constexpr int MIN_LEVEL = 1;
constexpr int MAX_LEVEL = 5;
constexpr int DEFAULT_LEVEL = 3;

// A stepper level to the setting it stands for, clamped to the levels' range.
auto densityFromLevel(int level) -> QrDensity;
auto speedFromLevel(int level) -> QrSpeed;
auto contrastFromLevel(int level) -> QrContrast;
auto cameraContrastFromLevel(int level) -> QrCameraContrast;
// The level a camera contrast stands at, for Camera Settings to show it.
auto cameraContrastLevel(QrCameraContrast contrast) -> int;

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
