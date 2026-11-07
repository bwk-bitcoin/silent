#pragma once

#include <QByteArray>

#include <optional>

#include "service.h"
#include "types.h"

namespace backend {

class ITransaction : public IService {
    Q_OBJECT

public:
    using IService::IService;
    ~ITransaction() override = default;

    [[nodiscard]] virtual auto craft(const TransactionTemplate &transaction) const
        -> CraftResult = 0;

    [[nodiscard]] virtual auto finalize(const TransactionTemplate &transaction) const
        -> std::optional<QByteArray> = 0;

public slots:
    virtual void selectCoins(backend::RequestId request_id,
                             backend::TransactionTemplate transaction) = 0;

signals:
    void coinsSelected(backend::RequestId request_id, backend::CraftResult result);
};

} // namespace backend

Q_DECLARE_INTERFACE(backend::ITransaction, "dev.silent.backend.ITransaction/1.0")
