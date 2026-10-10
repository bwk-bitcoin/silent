#pragma once

#include <QList>
#include <QString>
#include <QWidget>

#include <cstdint>

#include <silent.h>

namespace qontrol {
class Column;
}

namespace catalog {
class SelectRow;
}

namespace keys::flow {

// Where a key stands in the flow; it picks the row's pill, line and whether it can be
// picked.
enum class SignerStatus : std::uint8_t {
    First,
    Ready,
    Signed,
    Waiting,
    Disconnected,
    NoHot,
    NoRoute,
    Unregistered
};

// One key the request needs.
struct FlowRow {
    QString fingerprint;
    QString alias;
    // Unknown when the key has no route to show; a disconnected key keeps its USB one.
    KeyRouteKind route = KeyRouteKind::Unknown;
    // The live device's host instance id, for a Usb route.
    QString device_id;
    // The device as people know it ("Ledger"), empty when unknown.
    QString device;
    SignerStatus status = SignerStatus::NoRoute;

    [[nodiscard]] auto usable() const -> bool;
};

// A connected device that is not one of the request's keys.
struct OtherDevice {
    QString name;
    QString fingerprint;
};

// The picker: one row per key the request needs, an empty state when there is none,
// then the connected devices that cannot help.
class PickerStep : public QWidget {
    Q_OBJECT

public:
    explicit PickerStep(QWidget *parent = nullptr);
    // `lead` heads the rows, `empty` explains an empty list and `other_sub` says why
    // the other devices cannot help.
    void setRows(const QString &lead, const QList<FlowRow> &rows, const QList<OtherDevice> &others,
                 const QString &empty, const QString &other_sub);

signals:
    void rowPicked(int index);

public slots:
    void onRowClicked();

protected:
    [[nodiscard]] auto buildRow(const FlowRow &row, int index) const -> catalog::SelectRow *;
    [[nodiscard]] static auto buildOtherRow(const OtherDevice &device, const QString &sub)
        -> catalog::SelectRow *;

private:
    qontrol::Column *m_column = nullptr;
};

} // namespace keys::flow
