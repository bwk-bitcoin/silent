#pragma once

#include <interfaces/types.h>
#include <silent.h>
#include <string>

namespace account {

inline auto selectedInputs(const QList<plugin::SelectedInput> &inputs)
    -> rust::Vec<RustSelectedInput> {
    rust::Vec<RustSelectedInput> out;
    for (const auto &selected : inputs) {
        const auto &variant = selected.variant;
        RustSpendVariant path;
        path.path_id = variant.pathId.toStdString();
        path.keychain = variant.keychain;
        path.derivation_index = variant.derivationIndex;
        path.display = variant.display.toStdString();
        path.psbt_v2_hex = variant.psbtV2Hex.toStdString();
        path.descriptor = variant.descriptor.toStdString();
        path.sp_spend_key = variant.spSpendKey.toStdString();
        path.satisfaction_wu = variant.satisfactionWu;
        path.absolute_lock = {.kind = variant.absoluteLock.kind,
                              .value = variant.absoluteLock.value};
        path.relative_lock = {.kind = variant.relativeLock.kind,
                              .value = variant.relativeLock.value};
        path.maturity = variant.maturity;
        path.availability = variant.availability;
        for (const auto &key : variant.requiredKeys) {
            path.required_keys.push_back(key.toStdString());
        }
        for (const auto &preimage : variant.requiredPreimages) {
            path.required_preimages.push_back(preimage.toStdString());
        }
        out.push_back({.input_index = selected.inputIndex,
                       .network = selected.network,
                       .variant = std::move(path)});
    }
    return out;
}

inline auto spendingCoin(const RustSpendCoin &coin) -> plugin::Coin {
    plugin::Coin out;
    out.outpoint = QString::fromStdString(std::string(coin.outpoint));
    out.accountType = QString::fromStdString(std::string(coin.account_type));
    out.network = coin.network;
    out.value = coin.value;
    out.height = static_cast<quint32>(coin.height);
    if (coin.has_height) {
        out.confirmationHeight = coin.height;
    }
    out.label = QString::fromStdString(std::string(coin.label));
    out.status = coin.status;
    out.spent = coin.status == SpendCoinStatus::Spent || coin.status == SpendCoinStatus::BeingSpent;
    for (const auto &variant : coin.variants) {
        plugin::SpendVariant path;
        path.pathId = QString::fromStdString(std::string(variant.path_id));
        path.keychain = variant.keychain;
        path.derivationIndex = variant.derivation_index;
        path.display = QString::fromStdString(std::string(variant.display));
        path.psbtV2Hex = QString::fromStdString(std::string(variant.psbt_v2_hex));
        path.descriptor = QString::fromStdString(std::string(variant.descriptor));
        path.spSpendKey = QString::fromStdString(std::string(variant.sp_spend_key));
        for (const auto &key : variant.required_keys) {
            path.requiredKeys.append(QString::fromStdString(std::string(key)));
        }
        for (const auto &preimage : variant.required_preimages) {
            path.requiredPreimages.append(QString::fromStdString(std::string(preimage)));
        }
        path.satisfactionWu = variant.satisfaction_wu;
        path.absoluteLock = {.kind = variant.absolute_lock.kind,
                             .value = variant.absolute_lock.value};
        path.relativeLock = {.kind = variant.relative_lock.kind,
                             .value = variant.relative_lock.value};
        path.maturity = variant.maturity;
        path.availability = variant.availability;
        out.variants.append(path);
    }
    return out;
}

} // namespace account
