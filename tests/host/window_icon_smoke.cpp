#include "theme/Icon.h"

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QRect>
#include <QSize>

#include <algorithm>
#include <cstdlib>

namespace smoke {
namespace {

void require(bool passed, const char *context) {
    if (!passed) {
        qFatal("window icon: %s", context);
    }
}

// The buffer Qt 6.9 and later send a Wayland compositor for one window icon size, as
// QWaylandXdgToplevelIconV1::addPixmap fills it: a straight alpha image the pixmap is
// drawn into, announced as premultiplied ARGB8888.
auto waylandBuffer(const QIcon &icon, const QSize &size) -> QImage {
    const QPixmap pixmap = icon.pixmap(size, 1.0);
    QImage buffer(pixmap.size(), QImage::Format_ARGB32);
    buffer.fill(0);
    {
        QPainter painter(&buffer);
        painter.drawPixmap(pixmap.rect(), pixmap, pixmap.rect());
    }
    return buffer;
}

// The largest channel difference, over `background`, between the compositor drawing
// `buffer` as premultiplied and the mark drawn as it should be.
auto compositeError(const QImage &buffer, const QImage &mark, const QColor &background) -> int {
    int error = 0;
    for (int y = 0; y < mark.height(); ++y) {
        for (int x = 0; x < mark.width(); ++x) {
            const QRgb read = buffer.pixel(x, y);
            const QRgb want = mark.pixel(x, y);
            const int read_alpha = qAlpha(read);
            const int want_alpha = qAlpha(want);
            const int channels[3][3] = {
                {qRed(read), qRed(want), background.red()},
                {qGreen(read), qGreen(want), background.green()},
                {qBlue(read), qBlue(want), background.blue()},
            };
            for (const auto &channel : channels) {
                // Premultiplied over the background, as the compositor blends.
                const int drawn =
                    std::min(255, channel[0] + (((255 - read_alpha) * channel[2]) + 127) / 255);
                const int correct =
                    ((channel[1] * want_alpha) + (channel[2] * (255 - want_alpha)) + 127) / 255;
                error = std::max(error, std::abs(drawn - correct));
            }
        }
    }
    return error;
}

auto maxError(const QIcon &icon) -> int {
    int error = 0;
    for (const QSize &size : icon.availableSizes()) {
        QImage mark(size, QImage::Format_ARGB32_Premultiplied);
        mark.fill(Qt::transparent);
        {
            QPainter painter(&mark);
            icon::silentMark(IconColor::Accent).paint(&painter, mark.rect());
        }
        mark = mark.convertToFormat(QImage::Format_ARGB32);
        const QImage buffer = waylandBuffer(icon, size);
        require(buffer.size() == mark.size(), "a buffer is not at its icon size");
        for (const QColor &background : {QColor(0xef, 0xf0, 0xf1), QColor(0x2a, 0x2e, 0x32)}) {
            error = std::max(error, compositeError(buffer, mark, background));
        }
    }
    return error;
}

} // namespace

void windowIconSmoke() {
    const QIcon straight = icon::windowIcon(false);
    const QIcon premultiplied = icon::windowIcon(true);
    require(straight.availableSizes().size() == 7, "the window icon has not its 7 sizes");
    require(premultiplied.availableSizes() == straight.availableSizes(),
            "the premultiplied window icon has other sizes");
    for (const QSize &size : straight.availableSizes()) {
        require(straight.pixmap(size, 1.0).size() == size, "a window icon size is rescaled");
    }
    // The offscreen platform is not Wayland: the icon goes as it is.
    require(!icon::windowIconPremultiplied(), "offscreen asks for a premultiplied window icon");

    // Through the Wayland buffer, the straight icon's edges come out far too bright, the
    // premultiplied one's as drawn, within rounding: a pixmap keeps its colours
    // premultiplied, so a nearly transparent pixel (alpha about 11) is off by up to 12 on
    // a channel.
    const int straight_error = maxError(straight);
    const int premultiplied_error = maxError(premultiplied);
    qInfo("window icon: wayland composite error straight %d, premultiplied %d", straight_error,
          premultiplied_error);
    require(straight_error > 40, "the straight icon composites right: the check sees no bug");
    require(premultiplied_error <= 12, "the premultiplied icon composites wrong");
}

} // namespace smoke
