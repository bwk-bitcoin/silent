#include "KeysView.h"

#include "KeysModule.h"

#include "catalog/Variants.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"

namespace keys {

auto readyDevice(const QString &fingerprint) -> std::optional<plugin::SignerEntry> {
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && entry.signer->state() == plugin::SignerState::Ready &&
            QString::compare(entry.signer->fingerprint(), fingerprint, Qt::CaseInsensitive) == 0) {
            return entry;
        }
    }
    return std::nullopt;
}

auto keyStatusPill(const KeyInfo &info) -> catalog::Badge * {
    if (readyDevice(KeysModule::toQt(info.fingerprint)).has_value()) {
        return catalog::pillKeyConnected();
    }
    if (info.has_hot_secret) {
        return catalog::pillKeyHot();
    }
    return catalog::pillKeyOffline();
}

auto networkLabel(Network network) -> QString {
    switch (network) {
    case Network::Bitcoin:
        return TR("network-bitcoin");
    case Network::Testnet:
        return TR("network-testnet");
    case Network::Signet:
        return TR("network-signet");
    case Network::Regtest:
    default:
        return TR("network-regtest");
    }
}

} // namespace keys
