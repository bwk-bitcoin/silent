#include "QrCode.h"

#include "theme/Palette.h"
#include "theme/Theme.h"

#include <QDebug>
#include <QImage>
#include <QPainter>
#include <silent.h>
#include <string>

namespace catalog {

namespace {

constexpr qreal DIMMED_OPACITY = 0.25;

} // namespace

QrCode::QrCode(QWidget *parent)
    : QWidget(parent),
      m_padding(resolve(Padding::S)),
      m_radius(radius::BUTTON) {
    setProperty("class", "qr-code");
    setMinimumSize(resolve(Size::M), resolve(Size::M));
}

void QrCode::setData(const QString &data) {
    m_data = data;
    m_image = QImage();
    if (!data.isEmpty()) {
        auto text = data.toStdString();
        auto result = ::qr_text_image(rust::Str(text));
        if (result.is_ok) {
            // The result owns the pixels: copy them before it goes away.
            m_image = QImage(result.data.data(), static_cast<int>(result.width),
                             static_cast<int>(result.height), static_cast<int>(result.width),
                             QImage::Format_Grayscale8)
                          .copy();
        } else {
            qWarning() << "QrCode:" << QString::fromStdString(std::string(result.error));
        }
    }
    update();
}

void QrCode::setImage(const QImage &image) {
    m_image = image;
    update();
}

auto QrCode::data() const -> QString {
    return m_data;
}

void QrCode::setBackground(const QColor &background) {
    m_background = background;
    update();
}

void QrCode::setPadding(int padding) {
    m_padding = padding;
    update();
}

void QrCode::setRadius(int radius) {
    m_radius = radius;
    update();
}

void QrCode::setDimmed(bool dimmed) {
    m_dimmed = dimmed;
    update();
}

void QrCode::paintEvent([[maybe_unused]] QPaintEvent *event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (m_dimmed) {
        painter.setOpacity(DIMMED_OPACITY);
    }

    const auto &palette = Theme::get()->palette();
    QRect rect = this->rect().adjusted(m_padding, m_padding, -m_padding, -m_padding);
    int side = qMin(rect.width(), rect.height());
    QRect square(rect.x(), rect.y(), side, side);
    bool tuned = m_background.isValid();

    painter.setPen(QPen(palette.borderLight, 1));
    painter.setBrush(tuned ? m_background : palette.surface);
    painter.drawRoundedRect(this->rect().adjusted(1, 1, -1, -1), m_radius, m_radius);

    if (!tuned) {
        painter.setPen(QPen(palette.border, 1));
        painter.setBrush(Qt::white);
        painter.drawRect(square);
    }

    if (!m_image.isNull()) {
        painter.drawImage(square, m_image);
    }
}

auto QrCode::sizeHint() const -> QSize {
    return {resolve(Size::M), resolve(Size::M)};
}

} // namespace catalog
