#include "qr/bbqr/BbqrDisplayEngine.h"

#include <QImage>

#include <cstdint>
#include <memory>
#include <string>

#include <silent.h>

namespace qr {

namespace {

// The lowest density packs the least, most scannable frames; each level up packs more
// bytes and so fewer frames.
constexpr std::uint32_t MIN_PART_BYTES = 50;
constexpr std::uint32_t PART_BYTES_STEP = 50;

class BbqrFramer final : public QrFramer {
public:
    [[nodiscard]] auto render(const QByteArray &payload, int density) const -> QrRender override {
        auto partBytes =
            MIN_PART_BYTES + static_cast<std::uint32_t>(density - MIN_LEVEL) * PART_BYTES_STEP;
        auto frames = ::bbqr_frames(
            rust::Slice<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(payload.data()),
                                            payload.size()),
            partBytes);
        QrRender render;
        if (!frames->is_ok()) {
            render.error = QString::fromStdString(std::string(frames->get_error()));
            return render;
        }
        for (std::uint32_t index = 0; index < frames->count(); ++index) {
            auto frame = frames->frame(index);
            if (!frame.is_ok) {
                render.frames.clear();
                render.error = QString::fromStdString(std::string(frame.error));
                return render;
            }
            render.frames.append(QImage(frame.data.data(), static_cast<int>(frame.width),
                                        static_cast<int>(frame.height),
                                        static_cast<int>(frame.width), QImage::Format_Grayscale8)
                                     .copy());
        }
        return render;
    }
};

} // namespace

BbqrDisplayEngine::BbqrDisplayEngine(const QrDisplaySettings &settings, QObject *parent)
    : QrDisplayEngine(std::make_shared<BbqrFramer>(), settings, parent) {
}

} // namespace qr
