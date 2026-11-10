#pragma once

#include <QByteArray>

#include <optional>

#include "service.h"
#include "types.h"

namespace backend {

// Builds the account's transactions from templates: checking them, picking their coins and
// making their unsigned PSBTs. Signing is the account's own, not a service's.
class ITransaction : public IService {
    Q_OBJECT

public:
    using IService::IService;
    ~ITransaction() override = default;

    // Checks `transaction` against its own inputs, picking none: incomplete without inputs.
    // Local and pure: no persistence or network.
    [[nodiscard]] virtual auto craft(const TransactionTemplate &transaction) const
        -> CraftResult = 0;

    // The serialized unsigned PSBTv2 of a complete `transaction`, std::nullopt when it builds
    // none: a built-in account then answers error() with request id 0, during the call,
    // saying why. Changes no account state.
    [[nodiscard]] virtual auto finalize(const TransactionTemplate &transaction) const
        -> std::optional<QByteArray> = 0;

public slots:
    // Replaces the inputs of `transaction` by the coins coin selection picks, answered by
    // coinsSelected().
    virtual void selectCoins(backend::RequestId request_id,
                             backend::TransactionTemplate transaction) = 0;

signals:
    // Answers selectCoins(): incomplete, its own inputs kept, when no selection pays it.
    void coinsSelected(backend::RequestId request_id, backend::CraftResult result);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::ITransaction, "dev.silent.backend.ITransaction/1.0")
