#include "qr/bbqr/BbqrDisplayEngine.h"

#include <QImage>

#include <cstdint>
#include <string>

#include <silent.h>

namespace qr {

BbqrDisplayEngine::BbqrDisplayEngine(QObject *parent) : QrDisplayEngine(parent) {
}

BbqrDisplayEngine::~BbqrDisplayEngine() {
    stopWorker();
}

auto BbqrDisplayEngine::render(const QByteArray &payload, QrDensity density) const -> QrRender {
    auto partBytes = static_cast<std::uint32_t>(density);
    auto frames =
        ::bbqr_frames(rust::Slice<const std::uint8_t>(
                          reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size()),
                      partBytes);
    QrRender result;
    if (!frames->is_ok()) {
        result.error = QString::fromStdString(std::string(frames->get_error()));
        return result;
    }
    for (std::uint32_t index = 0; index < frames->count(); ++index) {
        auto frame = frames->frame(index);
        if (!frame.is_ok) {
            result.frames.clear();
            result.error = QString::fromStdString(std::string(frame.error));
            return result;
        }
        result.frames.append(QImage(frame.data.data(), static_cast<int>(frame.width),
                                    static_cast<int>(frame.height), static_cast<int>(frame.width),
                                    QImage::Format_Grayscale8)
                                 .copy());
    }
    return result;
}

} // namespace qr
