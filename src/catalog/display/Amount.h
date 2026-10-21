#pragma once

#include "catalog/display/Label.h"
#include <QObject>
#include <QString>
#include <QWidget>
#include <cstdint>

class QMouseEvent;

namespace catalog {

// How every amount reads: in BTC, in the selected price feed's fiat, or hidden.
enum class AmountMode { Btc, Fiat, Hidden };

// An amount in the current mode: its number and its unit (empty when hidden).
struct AmountParts {
    QString value;
    QString unit;
};

// The one app-wide amount mode, cycled BTC, fiat, hidden by clicking any amount. Fiat
// takes the selected feed's currency and rate only: without a rate the cycle skips it,
// and amounts show BTC until a rate comes back.
class AmountDisplay : public QObject {
    Q_OBJECT

public:
    static auto get() -> AmountDisplay *;
    [[nodiscard]] auto mode() const -> AmountMode;
    void cycle();
    [[nodiscard]] auto parts(uint64_t sats) const -> AmountParts;
    // The amount as one line ("0.01000000 BTC", "640.00 EUR", "****").
    [[nodiscard]] auto text(uint64_t sats) const -> QString;
    // The amount in the selected fiat, whatever the mode; empty without a rate.
    [[nodiscard]] static auto fiatText(uint64_t sats) -> QString;
    // The tooltip of a clickable amount, naming the modes it cycles through.
    [[nodiscard]] static auto cycleToolTip() -> QString;

signals:
    void changed();

public slots:
    void onSelectedPriceChanged();

protected:
    explicit AmountDisplay(QObject *parent);

private:
    AmountMode m_mode = AmountMode::Btc;
};

// An amount as plain text in the current mode (table cells, summaries). Clicking it
// cycles the mode unless it is fixed to BTC.
class AmountLabel : public Label {
    Q_OBJECT

public:
    explicit AmountLabel(QWidget *parent = nullptr);
    void setSats(uint64_t sats, const QString &sign = QString());
    // Always BTC and not clickable (the Send panel).
    void setFixed(bool fixed);

public slots:
    void onModeChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    uint64_t m_sats = 0;
    QString m_sign;
    bool m_fixed = false;
};

// An amount set as a mono number and a muted unit, sized from caption to hero.
// Clicking it cycles the mode unless it is fixed to BTC.
class Amount : public QWidget {
    Q_OBJECT

public:
    enum class Size { Sm, Md, Lg, Hero };

    explicit Amount(Size size = Size::Md, QWidget *parent = nullptr);
    void setSats(uint64_t sats, const QString &sign = QString());
    void setTone(LabelTone tone);
    // Always BTC and not clickable (the Send panel).
    void setFixed(bool fixed);
    // Holds the width of the widest amount in any mode, so cycling never shifts it.
    void setReserve(bool reserve);

public slots:
    void onModeChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void applyReserve();

private:
    Label *m_value = nullptr;
    Label *m_unit = nullptr;
    uint64_t m_sats = 0;
    QString m_sign;
    bool m_fixed = false;
    bool m_reserve = false;
};

} // namespace catalog
