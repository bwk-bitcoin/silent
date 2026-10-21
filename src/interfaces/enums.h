#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace plugin {

enum class Network : std::uint32_t { Bitcoin = 0, Testnet = 1, Signet = 2, Regtest = 3 };
enum class KeyRouteKind : std::uint32_t { Unknown = 0, Hot = 1, Usb = 2, Qr = 3, SdCard = 4 };
enum class SpendLockKind : std::uint32_t {
    None = 0,
    AbsoluteHeight = 1,
    AbsoluteTime = 2,
    RelativeBlocks = 3,
    RelativeTime512 = 4
};
enum class SpendMaturity : std::uint32_t { Mature = 0, Immature = 1, Unknown = 2 };
enum class SpendAvailability : std::uint32_t {
    Available = 0,
    Immature = 1,
    Unknown = 2,
    Spent = 3
};
enum class SpendCoinStatus : std::uint32_t {
    Unconfirmed = 0,
    ConfirmedUnverified = 1,
    Confirmed = 2,
    BeingSpent = 3,
    Spent = 4
};
enum class SigningStage : std::uint32_t {
    Signatures = 0,
    MusigNonces = 1,
    MusigSignatures = 2,
    SpContributions = 3,
    VerifySpContributions = 4,
    CompleteSpOutputs = 5,
    SpCompleteAndSign = 6,
    Release = 7
};
enum class SigningProgress : std::uint32_t { Partial = 0, StageComplete = 1, Complete = 2 };
enum class SpendError : std::uint32_t {
    Other = 0,
    Unsupported = 1,
    InvalidRequest = 2,
    Integrity = 3,
    LostSession = 4,
    Permission = 5,
    Cancelled = 6
};
enum class RetirementStatus : std::uint32_t { Pending = 0, Retired = 1, Failed = 2 };
// MusigSession means the device lost its session; restart from round 1.
enum class SignError : std::uint32_t { Other = 0, MusigSession = 1, InvalidRequest = 2 };

namespace detail {
template <typename Enum> struct WireValues;

template <> struct WireValues<Network> {
    static constexpr std::array VALUES{Network::Bitcoin, Network::Testnet, Network::Signet,
                                       Network::Regtest};
};

template <> struct WireValues<KeyRouteKind> {
    static constexpr std::array VALUES{KeyRouteKind::Unknown, KeyRouteKind::Hot, KeyRouteKind::Usb,
                                       KeyRouteKind::Qr, KeyRouteKind::SdCard};
};

template <> struct WireValues<SpendLockKind> {
    static constexpr std::array VALUES{SpendLockKind::None, SpendLockKind::AbsoluteHeight,
                                       SpendLockKind::AbsoluteTime, SpendLockKind::RelativeBlocks,
                                       SpendLockKind::RelativeTime512};
};

template <> struct WireValues<SpendMaturity> {
    static constexpr std::array VALUES{SpendMaturity::Mature, SpendMaturity::Immature,
                                       SpendMaturity::Unknown};
};

template <> struct WireValues<SpendAvailability> {
    static constexpr std::array VALUES{SpendAvailability::Available, SpendAvailability::Immature,
                                       SpendAvailability::Unknown, SpendAvailability::Spent};
};

template <> struct WireValues<SpendCoinStatus> {
    static constexpr std::array VALUES{
        SpendCoinStatus::Unconfirmed, SpendCoinStatus::ConfirmedUnverified,
        SpendCoinStatus::Confirmed, SpendCoinStatus::BeingSpent, SpendCoinStatus::Spent};
};

template <> struct WireValues<SigningStage> {
    static constexpr std::array VALUES{SigningStage::Signatures,
                                       SigningStage::MusigNonces,
                                       SigningStage::MusigSignatures,
                                       SigningStage::SpContributions,
                                       SigningStage::VerifySpContributions,
                                       SigningStage::CompleteSpOutputs,
                                       SigningStage::SpCompleteAndSign,
                                       SigningStage::Release};
};

template <> struct WireValues<SigningProgress> {
    static constexpr std::array VALUES{SigningProgress::Partial, SigningProgress::StageComplete,
                                       SigningProgress::Complete};
};

template <> struct WireValues<SpendError> {
    static constexpr std::array VALUES{SpendError::Other,          SpendError::Unsupported,
                                       SpendError::InvalidRequest, SpendError::Integrity,
                                       SpendError::LostSession,    SpendError::Permission,
                                       SpendError::Cancelled};
};

template <> struct WireValues<RetirementStatus> {
    static constexpr std::array VALUES{RetirementStatus::Pending, RetirementStatus::Retired,
                                       RetirementStatus::Failed};
};

template <> struct WireValues<SignError> {
    static constexpr std::array VALUES{SignError::Other, SignError::MusigSession,
                                       SignError::InvalidRequest};
};
} // namespace detail

// Wire values are unsigned integers, not enum object bytes. nullopt is a protocol error.
template <typename Enum> auto enumFromWire(std::uint32_t value) -> std::optional<Enum> {
    for (auto candidate : detail::WireValues<Enum>::VALUES) {
        if (static_cast<std::uint32_t>(candidate) == value) {
            return candidate;
        }
    }
    return std::nullopt;
}

template <typename Enum> auto enumToWire(Enum value) -> std::optional<std::uint32_t> {
    auto wire = static_cast<std::uint32_t>(value);
    if (!enumFromWire<Enum>(wire).has_value()) {
        return std::nullopt;
    }
    return wire;
}

} // namespace plugin
