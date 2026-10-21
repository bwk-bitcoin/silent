#pragma once

#include <QLabel>
#include <QString>
#include <QWidget>
#include <cstdint>

class QPaintEvent;

namespace catalog {

class Label;

// Under this window width table dates drop the time.
const int DATE_COMPACT_BELOW = 1040;

// The way a transaction moves the account's coins, as the backend names it.
enum class TxDirection { Incoming, Outgoing, Self, Unknown };

auto txDirection(const QString &direction) -> TxDirection;
// "Received", "Sent", "Sent to self"; "Transaction" when the direction is unknown.
auto directionLabel(TxDirection direction) -> QString;

// A history row's direction: incoming arrow down-right, outgoing arrow up-right, self
// circular arrows. Hover names it; an unknown direction shows nothing.
class DirectionIcon : public QLabel {
    Q_OBJECT

public:
    explicit DirectionIcon(TxDirection direction, QWidget *parent = nullptr);
};

// Confirmations of a transaction at `height` against `tip`: 0 while unconfirmed,
// -1 when the tip is not known.
auto txConfirmations(uint32_t height, uint32_t tip) -> int;

// A confirmation meter: 6 dots, 2 wide by 3 tall, one lit per confirmation down the
// left column then up the right. Past 6, or with unknown confirmations, it keeps its
// slot and draws nothing.
class ConfirmDots : public QWidget {
    Q_OBJECT

public:
    explicit ConfirmDots(int confirmations, QWidget *parent = nullptr);
    void setConfirmations(int confirmations);
    [[nodiscard]] auto sizeHint() const -> QSize override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int m_confirmations = 0;
};

// A table date, the Unconfirmed pill while unconfirmed, a muted "-" when the date is
// not known. Compact drops the time. Hovering a date tells how long ago it was.
auto dateStamp(uint64_t timestamp, bool unconfirmed, bool compact) -> QWidget *;
// The date column width that fits a date (or the pill) in that mode.
auto dateColumnWidth(bool compact) -> int;
// Whether `widget`'s window is narrow enough for compact dates.
auto compactDates(const QWidget *widget) -> bool;
// "x hours ago" from a unix timestamp.
auto timeAgo(uint64_t timestamp) -> QString;

// An amount in the app-wide amount mode, a muted "Unknown" when the backend could not
// tell it.
auto amountLabel(uint64_t sats, bool known = true) -> Label *;
// A long hash shown as head…tail.
auto shortenHash(const QString &hash) -> QString;

} // namespace catalog
