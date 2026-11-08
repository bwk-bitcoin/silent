#pragma once

#include "account/InterfaceEnums.h"
#include <algorithm>
#include <interfaces/types.h>
#include <optional>
#include <silent.h>
#include <string>

namespace account {

inline auto validRequest(const plugin::SigningRequest &request) -> bool {
    if (!toBridge(request.network).has_value() || !toBridge(request.stage).has_value()) {
        return false;
    }
    return std::ranges::all_of(request.inputs, [](const plugin::SelectedInput &input) {
        const auto &variant = input.variant;
        return toBridge(input.network).has_value() &&
               toBridge(variant.absoluteLock.kind).has_value() &&
               toBridge(variant.relativeLock.kind).has_value() &&
               toBridge(variant.maturity).has_value() && toBridge(variant.availability).has_value();
    });
}

// Call only after validRequest() has accepted all public enum values.
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
        path.absolute_lock = {.kind = *toBridge(variant.absoluteLock.kind),
                              .value = variant.absoluteLock.value};
        path.relative_lock = {.kind = *toBridge(variant.relativeLock.kind),
                              .value = variant.relativeLock.value};
        path.maturity = *toBridge(variant.maturity);
        path.availability = *toBridge(variant.availability);
        for (const auto &key : variant.requiredKeys) {
            path.required_keys.push_back(key.toStdString());
        }
        for (const auto &preimage : variant.requiredPreimages) {
            path.required_preimages.push_back(preimage.toStdString());
        }
        out.push_back({.input_index = selected.inputIndex,
                       .network = *toBridge(selected.network),
                       .variant = std::move(path)});
    }
    return out;
}

inline auto spendingCoin(const RustSpendCoin &coin) -> plugin::Coin {
    plugin::Coin out;
    out.outpoint = QString::fromStdString(std::string(coin.outpoint));
    out.accountType = QString::fromStdString(std::string(coin.account_type));
    out.network = toPublic<plugin::Network>(coin.network);
    out.value = coin.value;
    out.height = static_cast<quint32>(coin.height);
    if (coin.has_height) {
        out.confirmationHeight = coin.height;
    }
    out.label = QString::fromStdString(std::string(coin.label));
    out.status = toPublic<plugin::SpendCoinStatus>(coin.status);
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
        path.absoluteLock = {.kind = toPublic<plugin::SpendLockKind>(variant.absolute_lock.kind),
                             .value = variant.absolute_lock.value};
        path.relativeLock = {.kind = toPublic<plugin::SpendLockKind>(variant.relative_lock.kind),
                             .value = variant.relative_lock.value};
        path.maturity = toPublic<plugin::SpendMaturity>(variant.maturity);
        path.availability = toPublic<plugin::SpendAvailability>(variant.availability);
        out.variants.append(path);
    }
    return out;
}

inline auto chainTip(const SpendCoinsResult &result) -> std::optional<plugin::ChainTip> {
    if (!result.has_tip) {
        return std::nullopt;
    }
    plugin::ChainTip tip;
    tip.height = result.tip.height;
    tip.hash = QString::fromStdString(std::string(result.tip.hash));
    if (result.tip.has_median_time_past) {
        tip.medianTimePast = result.tip.median_time_past;
    }
    return tip;
}

} // namespace account
