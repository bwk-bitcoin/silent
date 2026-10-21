#pragma once

#include "theme/Palette.h"
#include <QWidget>
#include <cstdint>

namespace catalog {

class Amount;
class Label;

// The balance atop Coins and History: the confirmed hero amount and, to its right,
// the unconfirmed one, each with its coin count. Both keep a reserved width so
// cycling the amount mode never moves them.
class BalanceHeader : public QWidget {
    Q_OBJECT

public:
    explicit BalanceHeader(QWidget *parent = nullptr);
    void setConfirmed(uint64_t sats, uint64_t count);
    void setUnconfirmed(uint64_t sats, uint64_t count);
    static auto qss(const Palette &p) -> QString;

private:
    Amount *m_confirmed = nullptr;
    Label *m_confirmed_count = nullptr;
    QWidget *m_unconfirmed_dot = nullptr;
    Amount *m_unconfirmed = nullptr;
    Label *m_unconfirmed_count = nullptr;
};

} // namespace catalog
