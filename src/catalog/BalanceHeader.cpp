#include "catalog/BalanceHeader.h"

#include "catalog/display/Amount.h"
#include "catalog/display/Label.h"
#include "i18n/Tr.h"

#include <QStyle>
#include <Qontrol>

namespace catalog {

namespace {

constexpr int EYEBROW_PX = 10;
constexpr int COUNT_PX = 12;
constexpr int DOT_SIZE = 6;
constexpr int HERO_GAP = 8;         // design 8px between the eyebrow and the hero
constexpr int UNCONFIRMED_GAP = 8;  // design 8px between dot, eyebrow and amount
constexpr int UNCONFIRMED_LIFT = 3; // design 3px: the small amount sits on the hero baseline

auto eyebrow(const QString &text, QWidget *parent) -> Label * {
    auto *label = new Label(text, LabelRole::Eyebrow, parent);
    auto eyebrowFont = label->font();
    eyebrowFont.setPixelSize(EYEBROW_PX);
    eyebrowFont.setWeight(font::DEMIBOLD);
    label->setFont(eyebrowFont);
    return label;
}

auto countLabel(QWidget *parent) -> Label * {
    auto *label = new Label(LabelRole::Body, parent);
    label->setTone(LabelTone::Secondary);
    auto countFont = label->font();
    countFont.setPixelSize(COUNT_PX);
    label->setFont(countFont);
    return label;
}

auto countText(uint64_t count) -> QString {
    return QString("(%1)").arg(count);
}

} // namespace

BalanceHeader::BalanceHeader(QWidget *parent) : QWidget(parent) {
    setProperty("class", "balance-header");

    m_confirmed = new Amount(Amount::Size::Hero, this);
    m_confirmed->setReserve(true);
    m_confirmed_count = countLabel(this);
    auto *heroRow = (new qontrol::Row)
                        ->spacing(resolve(Padding::XS))
                        ->push(m_confirmed, 0, Qt::AlignBottom)
                        ->push(m_confirmed_count, 0, Qt::AlignBottom);
    auto *hero = (new qontrol::Column)
                     ->spacing(HERO_GAP)
                     ->push(eyebrow(TR("balance-title"), this))
                     ->push(heroRow);

    m_unconfirmed_dot = new QWidget(this);
    m_unconfirmed_dot->setProperty("class", "bh-dot");
    m_unconfirmed_dot->setFixedSize(DOT_SIZE, DOT_SIZE);
    m_unconfirmed = new Amount(Amount::Size::Sm, this);
    m_unconfirmed->setReserve(true);
    m_unconfirmed_count = countLabel(this);
    auto *unconfirmed = (new qontrol::Row)
                            ->spacing(UNCONFIRMED_GAP)
                            ->push(m_unconfirmed_dot, 0, Qt::AlignVCenter)
                            ->push(eyebrow(TR("balance-unconfirmed"), this), 0, Qt::AlignVCenter)
                            ->push(m_unconfirmed, 0, Qt::AlignVCenter)
                            ->push(m_unconfirmed_count, 0, Qt::AlignVCenter)
                            ->margins(0, 0, 0, UNCONFIRMED_LIFT);

    (new qontrol::Row)
        ->margins(resolve(Padding::XXS), resolve(Padding::XXS), resolve(Padding::XXS),
                  resolve(Padding::M))
        ->spacing(resolve(Spacing::XL))
        ->push(hero, 0, Qt::AlignBottom)
        ->push(unconfirmed, 0, Qt::AlignBottom)
        ->pushSpacer()
        ->into(this);

    setConfirmed(0, 0);
    setUnconfirmed(0, 0);
}

void BalanceHeader::setConfirmed(uint64_t sats, uint64_t count) {
    m_confirmed->setSats(sats);
    m_confirmed_count->setText(countText(count));
}

void BalanceHeader::setUnconfirmed(uint64_t sats, uint64_t count) {
    bool zero = sats == 0;
    m_unconfirmed->setSats(sats, zero ? QString() : QStringLiteral("+"));
    m_unconfirmed->setTone(zero ? LabelTone::Muted : LabelTone::Default);
    m_unconfirmed_count->setText(countText(count));
    m_unconfirmed_dot->setProperty("zero", zero);
    m_unconfirmed_dot->style()->unpolish(m_unconfirmed_dot);
    m_unconfirmed_dot->style()->polish(m_unconfirmed_dot);
}

auto BalanceHeader::qss(const Palette &p) -> QString {
    return QString("QWidget[class=\"balance-header\"] { border-bottom: 1px solid %1; }"
                   "QWidget[class=\"bh-dot\"] { background: %2; border-radius: 3px; }"
                   "QWidget[class=\"bh-dot\"][zero=\"true\"] { background: %3; }")
        .arg(p.borderLight.name()) // %1
        .arg(p.warning.name())     // %2
        .arg(p.textMuted.name());  // %3
}

} // namespace catalog
