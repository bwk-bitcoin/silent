#include "qr/bbqr/BbqrCameraEngine.h"

#include <QDebug>
#include <QImage>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace qr {

BbqrCameraEngine::BbqrCameraEngine(const QrCameraSettings &settings, QObject *parent)
    : QrCameraEngine(settings, parent),
      m_scanner(::bbqr_scanner_new()) {
    if (!m_scanner->is_ok()) {
        qWarning() << "bbqr scanner:"
                   << QString::fromStdString(std::string(m_scanner->get_error()));
    }
}

BbqrCameraEngine::~BbqrCameraEngine() {
    stopWorker();
}

auto BbqrCameraEngine::scan(const QImage &image) -> QrScan {
    QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    // Rows are padded to 4 bytes in a QImage; the scanner reads them packed.
    auto rowBytes = static_cast<std::size_t>(rgb.width()) * 3;
    std::vector<std::uint8_t> packed(rowBytes * rgb.height());
    for (int row = 0; row < rgb.height(); ++row) {
        std::copy_n(rgb.constScanLine(row), rowBytes, packed.begin() + row * rowBytes);
    }
    auto result = m_scanner->scan(rust::Slice<const std::uint8_t>(packed.data(), packed.size()),
                                  static_cast<std::uint32_t>(rgb.width()),
                                  static_cast<std::uint32_t>(rgb.height()));
    QrScan out;
    if (!result.is_ok) {
        qWarning() << "bbqr scan:" << QString::fromStdString(std::string(result.error));
        return out;
    }
    out.seen = static_cast<int>(result.seen);
    out.total = static_cast<int>(result.total);
    for (const auto &payload : result.payloads) {
        out.payloads.append(QByteArray(reinterpret_cast<const char *>(payload.data.data()),
                                       static_cast<qsizetype>(payload.data.size())));
    }
    return out;
}

void BbqrCameraEngine::clear() {
    m_scanner->reset();
}

} // namespace qr
