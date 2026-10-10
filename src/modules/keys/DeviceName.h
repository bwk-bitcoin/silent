#pragma once

#include <QString>

class ISigner;

namespace keys {

// A signing device as people know it: brand and model ("Ledger", "BitBox02"), never
// the lowercase kind the device service reports.
auto deviceName(const QString &kind, const QString &model) -> QString;
auto deviceName(const ISigner &signer) -> QString;

} // namespace keys
