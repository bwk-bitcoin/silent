#pragma once

#include <QDebug>
#include <array>
#include <interfaces/enums.h>
#include <optional>
#include <silent.h>
#include <utility>

namespace account {

template <typename Public> struct EnumConversion;

template <> struct EnumConversion<plugin::Network> {
    static constexpr std::array VALUES{std::pair{plugin::Network::Bitcoin, Network::Bitcoin},
                                       std::pair{plugin::Network::Testnet, Network::Testnet},
                                       std::pair{plugin::Network::Signet, Network::Signet},
                                       std::pair{plugin::Network::Regtest, Network::Regtest}};
};

template <> struct EnumConversion<plugin::KeyRouteKind> {
    static constexpr std::array VALUES{
        std::pair{plugin::KeyRouteKind::Unknown, KeyRouteKind::Unknown},
        std::pair{plugin::KeyRouteKind::Hot, KeyRouteKind::Hot},
        std::pair{plugin::KeyRouteKind::Usb, KeyRouteKind::Usb},
        std::pair{plugin::KeyRouteKind::Qr, KeyRouteKind::Qr},
        std::pair{plugin::KeyRouteKind::SdCard, KeyRouteKind::SdCard}};
};

template <> struct EnumConversion<plugin::SpendLockKind> {
    static constexpr std::array VALUES{
        std::pair{plugin::SpendLockKind::None, SpendLockKind::None},
        std::pair{plugin::SpendLockKind::AbsoluteHeight, SpendLockKind::AbsoluteHeight},
        std::pair{plugin::SpendLockKind::AbsoluteTime, SpendLockKind::AbsoluteTime},
        std::pair{plugin::SpendLockKind::RelativeBlocks, SpendLockKind::RelativeBlocks},
        std::pair{plugin::SpendLockKind::RelativeTime512, SpendLockKind::RelativeTime512}};
};

template <> struct EnumConversion<plugin::SpendMaturity> {
    static constexpr std::array VALUES{
        std::pair{plugin::SpendMaturity::Mature, SpendMaturity::Mature},
        std::pair{plugin::SpendMaturity::Immature, SpendMaturity::Immature},
        std::pair{plugin::SpendMaturity::Unknown, SpendMaturity::Unknown}};
};

template <> struct EnumConversion<plugin::SpendAvailability> {
    static constexpr std::array VALUES{
        std::pair{plugin::SpendAvailability::Available, SpendAvailability::Available},
        std::pair{plugin::SpendAvailability::Immature, SpendAvailability::Immature},
        std::pair{plugin::SpendAvailability::Unknown, SpendAvailability::Unknown},
        std::pair{plugin::SpendAvailability::Spent, SpendAvailability::Spent}};
};

template <> struct EnumConversion<plugin::SpendCoinStatus> {
    static constexpr std::array VALUES{
        std::pair{plugin::SpendCoinStatus::Unconfirmed, SpendCoinStatus::Unconfirmed},
        std::pair{plugin::SpendCoinStatus::ConfirmedUnverified,
                  SpendCoinStatus::ConfirmedUnverified},
        std::pair{plugin::SpendCoinStatus::Confirmed, SpendCoinStatus::Confirmed},
        std::pair{plugin::SpendCoinStatus::BeingSpent, SpendCoinStatus::BeingSpent},
        std::pair{plugin::SpendCoinStatus::Spent, SpendCoinStatus::Spent}};
};

template <> struct EnumConversion<plugin::SigningStage> {
    static constexpr std::array VALUES{
        std::pair{plugin::SigningStage::Signatures, SigningStage::Signatures},
        std::pair{plugin::SigningStage::MusigNonces, SigningStage::MusigNonces},
        std::pair{plugin::SigningStage::MusigSignatures, SigningStage::MusigSignatures},
        std::pair{plugin::SigningStage::SpContributions, SigningStage::SpContributions},
        std::pair{plugin::SigningStage::VerifySpContributions, SigningStage::VerifySpContributions},
        std::pair{plugin::SigningStage::CompleteSpOutputs, SigningStage::CompleteSpOutputs},
        std::pair{plugin::SigningStage::SpCompleteAndSign, SigningStage::SpCompleteAndSign},
        std::pair{plugin::SigningStage::Release, SigningStage::Release}};
};

template <> struct EnumConversion<plugin::SigningProgress> {
    static constexpr std::array VALUES{
        std::pair{plugin::SigningProgress::Partial, SigningProgress::Partial},
        std::pair{plugin::SigningProgress::StageComplete, SigningProgress::StageComplete},
        std::pair{plugin::SigningProgress::Complete, SigningProgress::Complete}};
};

template <> struct EnumConversion<plugin::SpendError> {
    static constexpr std::array VALUES{
        std::pair{plugin::SpendError::Other, SpendError::Other},
        std::pair{plugin::SpendError::Unsupported, SpendError::Unsupported},
        std::pair{plugin::SpendError::InvalidRequest, SpendError::InvalidRequest},
        std::pair{plugin::SpendError::Integrity, SpendError::Integrity},
        std::pair{plugin::SpendError::LostSession, SpendError::LostSession},
        std::pair{plugin::SpendError::Permission, SpendError::Permission},
        std::pair{plugin::SpendError::Cancelled, SpendError::Cancelled}};
};

template <typename Public>
auto toBridge(Public value)
    -> std::optional<typename decltype(EnumConversion<Public>::VALUES)::value_type::second_type> {
    for (const auto &[publicValue, bridgeValue] : EnumConversion<Public>::VALUES) {
        if (value == publicValue) {
            return bridgeValue;
        }
    }
    return std::nullopt;
}

template <typename Public, typename Bridge> auto toPublic(Bridge value) -> Public {
    for (const auto &[publicValue, bridgeValue] : EnumConversion<Public>::VALUES) {
        if (value == bridgeValue) {
            return publicValue;
        }
    }
    // Only trusted Rust results enter this path; protocol input uses checked toBridge().
    qFatal("invalid internal bridge enum");
}

} // namespace account
