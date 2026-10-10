#include "catalog/BalanceHeader.h"

#include "catalog/display/Label.h"
#include "catalog/format.h"

#include <Qontrol>

namespace catalog {

BalanceHeader::BalanceHeader(QWidget *parent) : QWidget(parent) {
    setProperty("class", "balance-header");

    m_title = new Label("BALANCE", LabelRole::Caption, this);
    auto eyebrowFont = m_title->font();
    eyebrowFont.setFamily(font::MONO);
    eyebrowFont.setWeight(font::DEMIBOLD);
    eyebrowFont.setLetterSpacing(QFont::PercentageSpacing, 116); // design 0.16em eyebrow tracking
    m_title->setFont(eyebrowFont);

    m_confirmed = new Label("0.00000000", LabelRole::Title, this);
    auto heroFont = m_confirmed->font();
    heroFont.setFamily(font::MONO);
    heroFont.setLetterSpacing(QFont::PercentageSpacing, 98); // design -0.015em hero tracking
    m_confirmed->setFont(heroFont);

    m_unit = new Label("BTC", LabelRole::Mono, this);
    m_unit->setProperty("bhrole", "unit");
    m_coin_count = new Label("(0 coins)", LabelRole::Body, this);
    m_coin_count->setProperty("bhrole", "count");

    auto *hero = (new qontrol::Row)
                     ->spacing(resolve(Spacing::XS))
                     ->push(m_confirmed)
                     ->push(m_unit, 0, Qt::AlignBottom)
                     ->push(m_coin_count, 0, Qt::AlignBottom)
                     ->pushSpacer();

    // Unconfirmed indicator: dot + eyebrow + value, hidden when zero.
    auto *ucRow = new qontrol::Row;
    m_unconfirmed_row = ucRow;
    auto *dot = new QWidget(ucRow);
    dot->setProperty("class", "bh-dot");
    dot->setFixedSize(6, 6);
    auto *ucap = new Label("UNCONFIRMED", LabelRole::Caption, ucRow);
    auto ucapFont = ucap->font();
    ucapFont.setFamily(font::MONO);
    ucapFont.setWeight(font::DEMIBOLD);
    ucapFont.setLetterSpacing(QFont::PercentageSpacing, 110); // design 0.1em tracking
    ucap->setFont(ucapFont);
    m_unconfirmed = new Label(LabelRole::Mono, ucRow);

    ucRow->spacing(resolve(Spacing::XS))
        ->push(dot, 0, Qt::AlignVCenter)
        ->push(ucap)
        ->push(m_unconfirmed)
        ->pushSpacer();
    m_unconfirmed_row->setVisible(false);

    (new qontrol::Column)
        ->margins(resolve(Padding::XXS), resolve(Padding::XXS), resolve(Padding::XXS),
                  resolve(Padding::M))
        ->spacing(resolve(Spacing::XXS))
        ->push(m_title)
        ->push(hero)
        ->push(m_unconfirmed_row)
        ->into(this);
}

void BalanceHeader::setConfirmed(const QString &amount) {
    m_confirmed->setText(amount);
}

void BalanceHeader::setUnconfirmed(uint64_t sats) {
    m_unconfirmed_row->setVisible(sats > 0);
    m_unconfirmed->setText("+" + toBitcoin(sats));
}

void BalanceHeader::setCoinCount(int count) {
    m_coin_count->setText(QString("(%1 %2)").arg(count).arg(count == 1 ? "coin" : "coins"));
}

auto BalanceHeader::qss(const Palette &p) -> QString {
    return QString("QWidget[class=\"balance-header\"] { border-bottom: 1px solid %1; }"
                   "QLabel[bhrole=\"unit\"] { color: %2; }"
                   "QLabel[bhrole=\"count\"] { color: %3; }"
                   "QWidget[class=\"bh-dot\"] { background: %4; border-radius: 3px; }")
        .arg(p.borderLight.name())   // %1
        .arg(p.textMuted.name())     // %2
        .arg(p.textSecondary.name()) // %3
        .arg(p.warning.name());      // %4
}

} // namespace catalog
