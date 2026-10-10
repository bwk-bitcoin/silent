#include "Icon.h"
#include "Theme.h"
#include "resources/icon/arrow_down.h"
#include "resources/icon/arrow_up.h"
#include "resources/icon/bitcoin.h"
#include "resources/icon/check.h"
#include "resources/icon/chevron_down.h"
#include "resources/icon/chevron_left.h"
#include "resources/icon/chevron_right.h"
#include "resources/icon/circle_check.h"
#include "resources/icon/circle_x.h"
#include "resources/icon/coins.h"
#include "resources/icon/copy.h"
#include "resources/icon/download.h"
#include "resources/icon/eye.h"
#include "resources/icon/eye_off.h"
#include "resources/icon/folder.h"
#include "resources/icon/history.h"
#include "resources/icon/info.h"
#include "resources/icon/key_round.h"
#include "resources/icon/layout_dashboard.h"
#include "resources/icon/lock.h"
#include "resources/icon/lock_open.h"
#include "resources/icon/minus.h"
#include "resources/icon/network.h"
#include "resources/icon/pencil.h"
#include "resources/icon/plus.h"
#include "resources/icon/puzzle.h"
#include "resources/icon/qr_code.h"
#include "resources/icon/refresh_cw.h"
#include "resources/icon/sd_card.h"
#include "resources/icon/server.h"
#include "resources/icon/settings.h"
#include "resources/icon/shield.h"
#include "resources/icon/silent_mark.h"
#include "resources/icon/trash_2.h"
#include "resources/icon/triangle_alert.h"
#include "resources/icon/upload.h"
#include "resources/icon/usb.h"
#include "resources/icon/wallet.h"
#include "resources/icon/x.h"
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QSvgRenderer>

namespace {

auto resolveColor(IconColor color) -> QColor {
    auto *theme = Theme::get();
    if (theme == nullptr) {
        return {Qt::black};
    }
    const auto &p = theme->palette();
    switch (color) {
    case IconColor::Accent:
        return p.accent;
    case IconColor::Secondary:
        return p.textSecondary;
    case IconColor::Muted:
        return p.textMuted;
    case IconColor::Success:
        return p.success;
    case IconColor::Warning:
        return p.warning;
    case IconColor::Error:
        return p.error;
    case IconColor::Default:
        return p.text;
    }
    return p.text;
}

// Renders an embedded SVG with a palette-resolved color at paint time, so widgets
// follow theme changes (and the chosen tint) without re-creating icons.
class SvgIconEngine final : public QIconEngine {
public:
    SvgIconEngine(const char *svg, int stroke_width, IconColor color)
        : m_svg(svg),
          m_stroke_width(stroke_width),
          m_color(color) {
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State state) override {
        Q_UNUSED(mode);
        Q_UNUSED(state);
        QColor color = resolveColor(m_color);
        QString svg = QString::fromUtf8(m_svg);
        svg.replace(QStringLiteral("stroke=\"currentColor\""),
                    QStringLiteral("stroke=\"%1\"").arg(color.name()));
        svg.replace(QStringLiteral("fill=\"currentColor\""),
                    QStringLiteral("fill=\"%1\"").arg(color.name()));
        svg.replace(QStringLiteral("stroke-width=\"2\""),
                    QStringLiteral("stroke-width=\"%1\"").arg(m_stroke_width));
        QSvgRenderer renderer(svg.toUtf8());
        renderer.render(painter, QRectF(rect));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }

    [[nodiscard]] QIconEngine *clone() const override {
        return new SvgIconEngine(m_svg, m_stroke_width, m_color);
    }

private:
    const char *m_svg;
    int m_stroke_width;
    IconColor m_color;
};

auto defaultStroke() -> int {
    return Theme::get()->iconPalette().defaultIcon.strokeWidth;
}

} // namespace

auto renderIcon(const char *svg_data, int stroke_width, IconColor color) -> QIcon {
    return QIcon(new SvgIconEngine(svg_data, stroke_width, color));
}

namespace icon {

auto trash(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::TRASH_2, defaultStroke(), color);
}

auto close(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::X, defaultStroke(), color);
}

auto copy(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::COPY, defaultStroke(), color);
}

auto plus(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::PLUS, defaultStroke(), color);
}

auto minus(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::MINUS, defaultStroke(), color);
}

auto download(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::DOWNLOAD, defaultStroke(), color);
}

auto upload(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::UPLOAD, defaultStroke(), color);
}

auto chevronLeft(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::CHEVRON_LEFT, defaultStroke(), color);
}

auto chevronRight(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::CHEVRON_RIGHT, defaultStroke(), color);
}

auto chevronDown(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::CHEVRON_DOWN, defaultStroke(), color);
}

auto check(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::CHECK, defaultStroke(), color);
}

auto x(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::X, defaultStroke(), color);
}

auto shield(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::SHIELD, defaultStroke(), color);
}

auto wallet(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::WALLET, defaultStroke(), color);
}

auto server(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::SERVER, defaultStroke(), color);
}

auto network(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::NETWORK, defaultStroke(), color);
}

auto eyeOff(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::EYE_OFF, defaultStroke(), color);
}

auto lockOpen(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::LOCK_OPEN, defaultStroke(), color);
}

auto bitcoin(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::BITCOIN, defaultStroke(), color);
}

auto coins(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::COINS, defaultStroke(), color);
}

auto send(IconColor color) -> QIcon {
    return upload(color);
}

auto receive(IconColor color) -> QIcon {
    return download(color);
}

auto settings(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::SETTINGS, defaultStroke(), color);
}

auto pencil(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::PENCIL, defaultStroke(), color);
}

auto history(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::HISTORY, defaultStroke(), color);
}

auto folder(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::FOLDER, defaultStroke(), color);
}

auto usb(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::USB, defaultStroke(), color);
}

auto refresh(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::REFRESH_CW, defaultStroke(), color);
}

auto qr(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::QR_CODE, defaultStroke(), color);
}

auto layout(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::LAYOUT_DASHBOARD, defaultStroke(), color);
}

auto silentMark(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::SILENT_MARK, defaultStroke(), color);
}

auto key(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::KEY_ROUND, defaultStroke(), color);
}

auto lock(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::LOCK, defaultStroke(), color);
}

auto sdCard(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::SD_CARD, defaultStroke(), color);
}

auto arrowUp(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::ARROW_UP, defaultStroke(), color);
}

auto arrowDown(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::ARROW_DOWN, defaultStroke(), color);
}

auto eye(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::EYE, defaultStroke(), color);
}

auto alert(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::TRIANGLE_ALERT, defaultStroke(), color);
}

auto info(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::INFO, defaultStroke(), color);
}

auto circleCheck(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::CIRCLE_CHECK, defaultStroke(), color);
}

auto circleX(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::CIRCLE_X, defaultStroke(), color);
}

auto puzzle(IconColor color) -> QIcon {
    return renderIcon(embedded_icon::PUZZLE, defaultStroke(), color);
}

} // namespace icon
