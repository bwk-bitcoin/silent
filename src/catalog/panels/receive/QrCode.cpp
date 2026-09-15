#include "QrCode.h"

#include "theme/Palette.h"
#include "theme/Theme.h"

#include <QImage>
#include <QPainter>

namespace catalog {

QrCode::QrCode(QWidget *parent) : QWidget(parent) {
    setProperty("class", "qr-code");
    setMinimumSize(resolve(Size::M), resolve(Size::M));
}

void QrCode::setData(const QString &data) {
    m_data = data;
    update();
}

void QrCode::setImage(const QImage &image) {
    m_image = image;
    update();
}

auto QrCode::data() const -> QString {
    return m_data;
}

void QrCode::paintEvent([[maybe_unused]] QPaintEvent *event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const auto &palette = Theme::get()->palette();
    QRect rect = this->rect().adjusted(resolve(Padding::S), resolve(Padding::S), -resolve(Padding::S),
                                       -resolve(Padding::S));
    int side = qMin(rect.width(), rect.height());
    QRect square(rect.x(), rect.y(), side, side);

    painter.setPen(QPen(palette.borderLight, 1));
    painter.setBrush(palette.surface);
    painter.drawRoundedRect(this->rect().adjusted(1, 1, -1, -1), radius::BUTTON, radius::BUTTON);

    painter.setPen(QPen(palette.border, 1));
    painter.setBrush(Qt::white);
    painter.drawRect(square);

    if (!m_image.isNull()) {
        painter.drawImage(square, m_image);
    }
}

auto QrCode::sizeHint() const -> QSize {
    return {resolve(Size::M), resolve(Size::M)};
}

} // namespace catalog
