#include "catalog/display/Amount.h"

#include "catalog/format.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "theme/Palette.h"

#include <QCoreApplication>
#include <QFont>
#include <QFontMetrics>
#include <QLocale>
#include <QMouseEvent>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <optional>

namespace catalog {

namespace {

const QString BTC_UNIT = QStringLiteral("BTC");
const QString HIDDEN_AMOUNT = QStringLiteral("****");
constexpr int FIAT_DECIMALS = 2;
// Design reserve: room for 999.99999999 BTC or its fiat with a sign, and its unit.
constexpr int RESERVE_VALUE_CHARS = 14;
constexpr int RESERVE_UNIT_CHARS = 3;
constexpr int UNIT_MIN_PX = 11;
constexpr qreal HERO_TRACKING = 98.5; // design -0.015em

struct Quote {
    QString currency;
    double rate = 0.0;
};

// The selected feed's quote, when it has one.
auto selectedQuote() -> std::optional<Quote> {
    auto price = Host::get()->selectedPrice();
    if (!price.rate.has_value() || price.currency.isEmpty()) {
        return std::nullopt;
    }
    return Quote{.currency = price.currency, .rate = *price.rate};
}

auto fiatParts(uint64_t sats, const Quote &quote) -> AmountParts {
    double btc = static_cast<double>(sats) / SATS;
    return {.value = QLocale().toString(btc * quote.rate, 'f', FIAT_DECIMALS),
            .unit = quote.currency};
}

auto joined(const AmountParts &parts) -> QString {
    return parts.unit.isEmpty() ? parts.value : parts.value + ' ' + parts.unit;
}

auto valuePx(Amount::Size size) -> int {
    switch (size) {
    case Amount::Size::Sm:
        return 13;
    case Amount::Size::Md:
        return 15;
    case Amount::Size::Lg:
        return 19;
    case Amount::Size::Hero:
        return 30;
    }
    return 15;
}

} // namespace

auto AmountDisplay::get() -> AmountDisplay * {
    static auto *display = new AmountDisplay(qApp);
    return display;
}

AmountDisplay::AmountDisplay(QObject *parent) : QObject(parent) {
    connect(Host::get()->events(), &HostEvents::selectedPriceChanged, this,
            &AmountDisplay::onSelectedPriceChanged, qontrol::UNIQUE);
}

auto AmountDisplay::mode() const -> AmountMode {
    if (m_mode == AmountMode::Fiat && !selectedQuote().has_value()) {
        return AmountMode::Btc;
    }
    return m_mode;
}

void AmountDisplay::cycle() {
    switch (mode()) {
    case AmountMode::Btc:
        m_mode = selectedQuote().has_value() ? AmountMode::Fiat : AmountMode::Hidden;
        break;
    case AmountMode::Fiat:
        m_mode = AmountMode::Hidden;
        break;
    case AmountMode::Hidden:
        m_mode = AmountMode::Btc;
        break;
    }
    emit changed();
}

auto AmountDisplay::parts(uint64_t sats) const -> AmountParts {
    if (m_mode == AmountMode::Hidden) {
        return {.value = HIDDEN_AMOUNT};
    }
    if (m_mode == AmountMode::Fiat) {
        if (auto quote = selectedQuote()) {
            return fiatParts(sats, *quote);
        }
    }
    return {.value = toBitcoin(sats, false), .unit = BTC_UNIT};
}

auto AmountDisplay::text(uint64_t sats) const -> QString {
    return joined(parts(sats));
}

auto AmountDisplay::fiatText(uint64_t sats) -> QString {
    auto quote = selectedQuote();
    if (!quote.has_value()) {
        return {};
    }
    return joined(fiatParts(sats, *quote));
}

auto AmountDisplay::cycleToolTip() -> QString {
    auto quote = selectedQuote();
    if (!quote.has_value()) {
        return TR("amount-cycle-no-fiat");
    }
    return TR("amount-cycle").arg(quote->currency);
}

void AmountDisplay::onSelectedPriceChanged() {
    emit changed();
}

AmountLabel::AmountLabel(QWidget *parent) : Label(LabelRole::Mono, parent) {
    connect(AmountDisplay::get(), &AmountDisplay::changed, this, &AmountLabel::onModeChanged,
            qontrol::UNIQUE);
    onModeChanged();
}

void AmountLabel::setSats(uint64_t sats, const QString &sign) {
    m_sats = sats;
    m_sign = sign;
    onModeChanged();
}

void AmountLabel::setFixed(bool fixed) {
    m_fixed = fixed;
    onModeChanged();
}

void AmountLabel::onModeChanged() {
    auto *display = AmountDisplay::get();
    if (m_fixed) {
        setText(m_sign + toBitcoin(m_sats));
        setToolTip(QString());
        unsetCursor();
        return;
    }
    QString sign = display->mode() == AmountMode::Hidden ? QString() : m_sign;
    setText(sign + display->text(m_sats));
    setToolTip(AmountDisplay::cycleToolTip());
    setCursor(Qt::PointingHandCursor);
}

// Accepted: the click cycles the amounts and does not reach the row under it.
void AmountLabel::mousePressEvent(QMouseEvent *event) {
    if (m_fixed || event->button() != Qt::LeftButton) {
        Label::mousePressEvent(event);
        return;
    }
    event->accept();
}

void AmountLabel::mouseReleaseEvent(QMouseEvent *event) {
    if (m_fixed || event->button() != Qt::LeftButton) {
        Label::mouseReleaseEvent(event);
        return;
    }
    event->accept();
    if (rect().contains(event->position().toPoint())) {
        AmountDisplay::get()->cycle();
    }
}

Amount::Amount(Size size, QWidget *parent) : QWidget(parent) {
    int px = valuePx(size);

    m_value = new Label(LabelRole::Mono, this);
    auto valueFont = m_value->font();
    valueFont.setFamily(font::MONO);
    valueFont.setPixelSize(px);
    valueFont.setWeight(font::DEMIBOLD);
    if (size == Size::Hero) {
        valueFont.setLetterSpacing(QFont::PercentageSpacing, HERO_TRACKING);
    }
    m_value->setFont(valueFont);

    m_unit = new Label(LabelRole::Mono, this);
    m_unit->setTone(LabelTone::Muted);
    auto unitFont = m_unit->font();
    unitFont.setFamily(font::MONO);
    unitFont.setPixelSize(std::max(UNIT_MIN_PX, px / 2));
    m_unit->setFont(unitFont);

    (new qontrol::Row)
        ->spacing(resolve(Padding::XS))
        ->push(m_value, 0, Qt::AlignBottom)
        ->push(m_unit, 0, Qt::AlignBottom)
        ->into(this);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    connect(AmountDisplay::get(), &AmountDisplay::changed, this, &Amount::onModeChanged,
            qontrol::UNIQUE);
    onModeChanged();
}

void Amount::setSats(uint64_t sats, const QString &sign) {
    m_sats = sats;
    m_sign = sign;
    onModeChanged();
}

void Amount::setTone(LabelTone tone) {
    m_value->setTone(tone);
}

void Amount::setFixed(bool fixed) {
    m_fixed = fixed;
    onModeChanged();
}

void Amount::setReserve(bool reserve) {
    m_reserve = reserve;
    applyReserve();
    onModeChanged();
}

void Amount::applyReserve() {
    if (!m_reserve) {
        m_value->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_value->setMinimumWidth(0);
        m_unit->setMinimumWidth(0);
        return;
    }
    m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto valueMetrics = m_value->fontMetrics();
    auto unitMetrics = m_unit->fontMetrics();
    int valueWidth = std::max(valueMetrics.horizontalAdvance('0') * RESERVE_VALUE_CHARS,
                              valueMetrics.horizontalAdvance(m_sign + toBitcoin(m_sats, false)));
    int unitWidth = std::max(unitMetrics.horizontalAdvance('0') * RESERVE_UNIT_CHARS,
                             unitMetrics.horizontalAdvance(BTC_UNIT));
    if (auto quote = selectedQuote(); quote.has_value() && !m_fixed) {
        auto fiat = fiatParts(m_sats, *quote);
        valueWidth = std::max(valueWidth, valueMetrics.horizontalAdvance(m_sign + fiat.value));
        unitWidth = std::max(unitWidth, unitMetrics.horizontalAdvance(fiat.unit));
    }
    m_value->setMinimumWidth(valueWidth);
    m_unit->setMinimumWidth(unitWidth);
}

void Amount::onModeChanged() {
    applyReserve();
    auto *display = AmountDisplay::get();
    AmountParts parts{.value = toBitcoin(m_sats, false), .unit = BTC_UNIT};
    QString sign = m_sign;
    if (!m_fixed) {
        parts = display->parts(m_sats);
        if (display->mode() == AmountMode::Hidden) {
            sign.clear();
        }
    }
    m_value->setText(sign + parts.value);
    m_unit->setText(parts.unit);
    // Without a reserve a hidden amount has no unit to show.
    m_unit->setVisible(m_reserve || !parts.unit.isEmpty());
    setToolTip(m_fixed ? QString() : AmountDisplay::cycleToolTip());
    if (m_fixed) {
        unsetCursor();
    } else {
        setCursor(Qt::PointingHandCursor);
    }
}

// Accepted: the click cycles the amounts and does not reach the row under it.
void Amount::mousePressEvent(QMouseEvent *event) {
    if (m_fixed || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    event->accept();
}

void Amount::mouseReleaseEvent(QMouseEvent *event) {
    if (m_fixed || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    event->accept();
    if (rect().contains(event->position().toPoint())) {
        AmountDisplay::get()->cycle();
    }
}

} // namespace catalog
