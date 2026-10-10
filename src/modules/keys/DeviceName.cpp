#include "DeviceName.h"

#include "interfaces/signer.h"

#include <QHash>

namespace keys {

namespace {

// The kinds bwk-hwi reports (DeviceKind's Display). Brand names stay untranslated.
auto brandFor(const QString &kind) -> QString {
    static const QHash<QString, QString> brands = {
        {QStringLiteral("bitbox02"), QStringLiteral("BitBox02")},
        {QStringLiteral("coldcard"), QStringLiteral("Coldcard")},
        {QStringLiteral("specter"), QStringLiteral("Specter")},
        {QStringLiteral("specter-simulator"), QStringLiteral("Specter Simulator")},
        {QStringLiteral("ledger"), QStringLiteral("Ledger")},
        {QStringLiteral("ledger-simulator"), QStringLiteral("Ledger Simulator")},
        {QStringLiteral("jade"), QStringLiteral("Jade")},
    };
    return brands.value(kind, kind);
}

} // namespace

auto deviceName(const QString &kind, const QString &model) -> QString {
    return QStringLiteral("%1 %2").arg(brandFor(kind), model).trimmed();
}

auto deviceName(const ISigner &signer) -> QString {
    return deviceName(signer.brand(), signer.model());
}

} // namespace keys
