#include <interfaces/account.h>
#include <interfaces/host.h>
#include <interfaces/keys.h>
#include <interfaces/signer.h>
#include <interfaces/transactions.h>

#include <QCoreApplication>
#include <QDebug>
#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("public Keys contract: %s", message);
    }
}

template <typename Enum, std::size_t Size>
void wireValues(const std::array<std::pair<Enum, std::uint32_t>, Size> &values) {
    for (const auto &[value, wire] : values) {
        require(plugin::enumToWire(value) == wire, "encoding matches the defined integer");
        require(plugin::enumFromWire<Enum>(wire) == value, "decoding matches the defined enum");
    }
    for (auto invalid : {static_cast<std::uint32_t>(Size), 255U, 256U, 65536U,
                         std::numeric_limits<std::uint32_t>::max()}) {
        require(!plugin::enumFromWire<Enum>(invalid).has_value(), "unknown integer is rejected");
        require(!plugin::enumToWire(static_cast<Enum>(invalid)).has_value(),
                "invalid enum cannot be encoded");
    }
}

// Compile the external consumer's named callback and typed connection without the host.
class Consumer final : public QObject {
    Q_OBJECT
public:
    void attach(IKeys *keys) const {
        connect(keys, &IKeys::coordinationError, this, &Consumer::onError, Qt::UniqueConnection);
    }

public slots:

    void onError([[maybe_unused]] ReqId req_id, [[maybe_unused]] plugin::SpendError kind,
                 [[maybe_unused]] const QString &message) {
    }
};

static_assert(std::is_same_v<decltype(KeyEntry::route), plugin::KeyRouteKind>);
static_assert(std::is_same_v<decltype(plugin::Coin::network), plugin::Network>);
static_assert(std::is_same_v<decltype(plugin::SelectedInput::network), plugin::Network>);
static_assert(std::is_same_v<decltype(plugin::SigningRequest::network), plugin::Network>);
static_assert(
    std::is_same_v<decltype(&IKeys::keys), KeyListResult (IKeys::*)(plugin::Network) const>);
} // namespace

auto main(int argc, char **argv) -> int {
    QCoreApplication app(argc, argv);
    KeyEntry entry;
    require(plugin::validateKeyEntry(entry), "Unknown is a valid route, not a protocol failure");
    for (auto invalid : {256U, std::numeric_limits<std::uint32_t>::max()}) {
        entry.route = static_cast<plugin::KeyRouteKind>(invalid);
        require(!plugin::validateKeyEntry(entry),
                "public route validation rejects invalid integers");
        entry.route = plugin::KeyRouteKind::Unknown;
        entry.descriptor_route = static_cast<plugin::KeyRouteKind>(invalid);
        require(!plugin::validateKeyEntry(entry),
                "public descriptor route validation rejects invalid integers");
        entry.descriptor_route = plugin::KeyRouteKind::Unknown;
    }
    wireValues(std::array{
        std::pair{plugin::Network::Bitcoin, 0U}, std::pair{plugin::Network::Testnet, 1U},
        std::pair{plugin::Network::Signet, 2U}, std::pair{plugin::Network::Regtest, 3U}});
    wireValues(std::array{
        std::pair{plugin::KeyRouteKind::Unknown, 0U}, std::pair{plugin::KeyRouteKind::Hot, 1U},
        std::pair{plugin::KeyRouteKind::Usb, 2U}, std::pair{plugin::KeyRouteKind::Qr, 3U},
        std::pair{plugin::KeyRouteKind::SdCard, 4U}});
    wireValues(std::array{std::pair{plugin::SpendLockKind::None, 0U},
                          std::pair{plugin::SpendLockKind::AbsoluteHeight, 1U},
                          std::pair{plugin::SpendLockKind::AbsoluteTime, 2U},
                          std::pair{plugin::SpendLockKind::RelativeBlocks, 3U},
                          std::pair{plugin::SpendLockKind::RelativeTime512, 4U}});
    wireValues(std::array{std::pair{plugin::SpendMaturity::Mature, 0U},
                          std::pair{plugin::SpendMaturity::Immature, 1U},
                          std::pair{plugin::SpendMaturity::Unknown, 2U}});
    wireValues(std::array{std::pair{plugin::SpendAvailability::Available, 0U},
                          std::pair{plugin::SpendAvailability::Immature, 1U},
                          std::pair{plugin::SpendAvailability::Unknown, 2U},
                          std::pair{plugin::SpendAvailability::Spent, 3U}});
    wireValues(std::array{std::pair{plugin::SpendCoinStatus::Unconfirmed, 0U},
                          std::pair{plugin::SpendCoinStatus::ConfirmedUnverified, 1U},
                          std::pair{plugin::SpendCoinStatus::Confirmed, 2U},
                          std::pair{plugin::SpendCoinStatus::BeingSpent, 3U},
                          std::pair{plugin::SpendCoinStatus::Spent, 4U}});
    wireValues(std::array{std::pair{plugin::SigningStage::Signatures, 0U},
                          std::pair{plugin::SigningStage::MusigNonces, 1U},
                          std::pair{plugin::SigningStage::MusigSignatures, 2U},
                          std::pair{plugin::SigningStage::SpContributions, 3U},
                          std::pair{plugin::SigningStage::VerifySpContributions, 4U},
                          std::pair{plugin::SigningStage::CompleteSpOutputs, 5U},
                          std::pair{plugin::SigningStage::SpCompleteAndSign, 6U},
                          std::pair{plugin::SigningStage::Release, 7U}});
    wireValues(std::array{std::pair{plugin::SigningProgress::Partial, 0U},
                          std::pair{plugin::SigningProgress::StageComplete, 1U},
                          std::pair{plugin::SigningProgress::Complete, 2U}});
    wireValues(std::array{std::pair{plugin::SpendError::Other, 0U},
                          std::pair{plugin::SpendError::Unsupported, 1U},
                          std::pair{plugin::SpendError::InvalidRequest, 2U},
                          std::pair{plugin::SpendError::Integrity, 3U},
                          std::pair{plugin::SpendError::LostSession, 4U},
                          std::pair{plugin::SpendError::Permission, 5U},
                          std::pair{plugin::SpendError::Cancelled, 6U}});
    wireValues(std::array{std::pair{plugin::RetirementStatus::Pending, 0U},
                          std::pair{plugin::RetirementStatus::Retired, 1U},
                          std::pair{plugin::RetirementStatus::Failed, 2U}});
    wireValues(std::array{std::pair{plugin::SignError::Other, 0U},
                          std::pair{plugin::SignError::MusigSession, 1U},
                          std::pair{plugin::SignError::InvalidRequest, 2U}});
    qInfo("public Keys contract: OK");
    return 0;
}

#include "keys_consumer.moc"
