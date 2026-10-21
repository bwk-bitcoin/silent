#include "catalog/display/TxIndicators.h"

#include "catalog/Variants.h"
#include "catalog/display/Amount.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "i18n/Tr.h"
#include "resources/icon/arrow_down_right.h"
#include "resources/icon/arrow_up_right.h"
#include "theme/Icon.h"
#include "theme/Theme.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QLocale>
#include <QPainter>
#include <Qontrol>
#include <algorithm>
#include <array>

namespace catalog {

namespace {

const int DIRECTION_ICON_SIZE = 16;

const int DOT_SIZE = 5;
const int DOT_GAP = 3;
const int DOT_COLUMNS = 2;
const int DOT_ROWS = 3;
const int DOTS_MAX = DOT_COLUMNS * DOT_ROWS;
// The confirmation each grid cell lights at, row by row: down the left column, then up
// the right one.
constexpr std::array<int, DOTS_MAX> CONFIRM_ORDER = {1, 6, 2, 5, 3, 4};

const QString DATE_FORMAT = QStringLiteral("yyyy-MM-dd HH:mm");
const QString DATE_COMPACT_FORMAT = QStringLiteral("yyyy-MM-dd");

const int64_t HOUR_SECS = 3600;
const int64_t DAY_HOURS = 24;
const int64_t MONTH_DAYS = 30;
const int64_t YEAR_DAYS = 365;

const int HASH_SHORTEN_MIN = 16; // below this length a hash is shown in full
const int HASH_HEAD = 6;
const int HASH_TAIL = 6;

auto directionIcon(TxDirection direction) -> QIcon {
    int stroke = Theme::get()->iconPalette().defaultIcon.strokeWidth;
    switch (direction) {
    case TxDirection::Incoming:
        return renderIcon(embedded_icon::ARROW_DOWN_RIGHT, stroke, IconColor::Success);
    case TxDirection::Outgoing:
        return renderIcon(embedded_icon::ARROW_UP_RIGHT, stroke, IconColor::Error);
    case TxDirection::Self:
        return icon::refresh(IconColor::Accent);
    case TxDirection::Unknown:
        return {};
    }
    return {};
}

auto agoText(int64_t count, const QString &one, const QString &many) -> QString {
    return count == 1 ? one : many.arg(count);
}

} // namespace

auto txDirection(const QString &direction) -> TxDirection {
    if (direction == QStringLiteral("incoming")) {
        return TxDirection::Incoming;
    }
    if (direction == QStringLiteral("outgoing")) {
        return TxDirection::Outgoing;
    }
    if (direction == QStringLiteral("self")) {
        return TxDirection::Self;
    }
    return TxDirection::Unknown;
}

auto directionLabel(TxDirection direction) -> QString {
    switch (direction) {
    case TxDirection::Incoming:
        return TR("tx-direction-received");
    case TxDirection::Outgoing:
        return TR("tx-direction-sent");
    case TxDirection::Self:
        return TR("tx-direction-self");
    case TxDirection::Unknown:
        return TR("tx-direction-unknown");
    }
    return TR("tx-direction-unknown");
}

DirectionIcon::DirectionIcon(TxDirection direction, QWidget *parent) : QLabel(parent) {
    setFixedSize(DIRECTION_ICON_SIZE, DIRECTION_ICON_SIZE);
    if (direction == TxDirection::Unknown) {
        return;
    }
    setPixmap(directionIcon(direction).pixmap(DIRECTION_ICON_SIZE, DIRECTION_ICON_SIZE));
    setToolTip(directionLabel(direction));
}

auto txConfirmations(uint32_t height, uint32_t tip) -> int {
    if (height == 0) {
        return 0;
    }
    if (tip == 0 || tip < height) {
        return -1;
    }
    return static_cast<int>(tip - height + 1);
}

ConfirmDots::ConfirmDots(int confirmations, QWidget *parent) : QWidget(parent) {
    setFixedSize(sizeHint());
    setConfirmations(confirmations);
}

void ConfirmDots::setConfirmations(int confirmations) {
    m_confirmations = confirmations;
    if (confirmations < 0) {
        setToolTip({});
    } else if (confirmations == 0) {
        setToolTip(TR("tx-unconfirmed"));
    } else if (confirmations == 1) {
        setToolTip(TR("tx-confirmations-one"));
    } else {
        setToolTip(TR("tx-confirmations").arg(QLocale().toString(confirmations)));
    }
    update();
}

auto ConfirmDots::sizeHint() const -> QSize {
    return {(DOT_COLUMNS * DOT_SIZE) + ((DOT_COLUMNS - 1) * DOT_GAP),
            (DOT_ROWS * DOT_SIZE) + ((DOT_ROWS - 1) * DOT_GAP)};
}

void ConfirmDots::paintEvent([[maybe_unused]] QPaintEvent *event) {
    if (m_confirmations < 0 || m_confirmations > DOTS_MAX) {
        return;
    }
    const auto &p = Theme::get()->palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < DOTS_MAX; ++i) {
        int x = (i % DOT_COLUMNS) * (DOT_SIZE + DOT_GAP);
        int y = (i / DOT_COLUMNS) * (DOT_SIZE + DOT_GAP);
        painter.setBrush(CONFIRM_ORDER.at(i) <= m_confirmations ? p.accent : p.border);
        painter.drawEllipse(x, y, DOT_SIZE, DOT_SIZE);
    }
}

auto dateStamp(uint64_t timestamp, bool unconfirmed, bool compact) -> QWidget * {
    if (unconfirmed) {
        // As wide as the column, so the pill sits in its middle.
        auto *pill = (new qontrol::Row)->pushSpacer()->push(pillTxUnconfirmed())->pushSpacer();
        pill->setFixedWidth(dateColumnWidth(compact));
        return pill;
    }
    if (timestamp == 0) {
        auto *dash = new Label(QStringLiteral("-"), LabelRole::Mono);
        dash->setTone(LabelTone::Muted);
        return dash;
    }
    auto date = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp));
    auto *label =
        new Label(date.toString(compact ? DATE_COMPACT_FORMAT : DATE_FORMAT), LabelRole::Mono);
    label->setToolTip(timeAgo(timestamp));
    return label;
}

auto dateColumnWidth(bool compact) -> int {
    Label sample(LabelRole::Mono);
    auto sampleDate = QDateTime(QDate(2000, 12, 31), QTime(23, 59));
    int dateWidth =
        QFontMetrics(sample.font())
            .horizontalAdvance(sampleDate.toString(compact ? DATE_COMPACT_FORMAT : DATE_FORMAT));
    auto *pill = pillTxUnconfirmed();
    int pillWidth = pill->sizeHint().width();
    delete pill;
    return std::max(dateWidth, pillWidth);
}

auto compactDates(const QWidget *widget) -> bool {
    return widget != nullptr && widget->window()->width() < DATE_COMPACT_BELOW;
}

auto timeAgo(uint64_t timestamp) -> QString {
    int64_t hours =
        (QDateTime::currentSecsSinceEpoch() - static_cast<int64_t>(timestamp)) / HOUR_SECS;
    if (hours < 1) {
        return TR("time-ago-under-hour");
    }
    if (hours < DAY_HOURS) {
        return agoText(hours, TR("time-ago-hours-one"), TR("time-ago-hours"));
    }
    int64_t days = hours / DAY_HOURS;
    if (days < MONTH_DAYS) {
        return agoText(days, TR("time-ago-days-one"), TR("time-ago-days"));
    }
    if (days < YEAR_DAYS) {
        return agoText(days / MONTH_DAYS, TR("time-ago-months-one"), TR("time-ago-months"));
    }
    return agoText(days / YEAR_DAYS, TR("time-ago-years-one"), TR("time-ago-years"));
}

auto amountLabel(uint64_t sats, bool known) -> Label * {
    if (!known) {
        auto *unknown = new Label(TR("tx-unknown"), LabelRole::Mono);
        unknown->setTone(LabelTone::Muted);
        return unknown;
    }
    auto *amount = new AmountLabel;
    amount->setSats(sats);
    return amount;
}

auto shortenHash(const QString &hash) -> QString {
    if (hash.length() < HASH_SHORTEN_MIN) {
        return hash;
    }
    return hash.left(HASH_HEAD) + QStringLiteral("…") + hash.right(HASH_TAIL);
}

} // namespace catalog
