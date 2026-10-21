#pragma once

#include <QThread>
#include <interfaces/types.h>
#include <silent.h>

class TransactionJob final : public QThread {
public:
    enum class Operation { Finalize, Broadcast, Release };
    TransactionJob(ReqId req_id, Operation operation, plugin::SigningRequest request,
                   QString endpoint = {}, QString tx_hex = {});

    [[nodiscard]] auto requestId() const -> ReqId {
        return m_req_id;
    }

    [[nodiscard]] auto operation() const -> Operation {
        return m_operation;
    }

    [[nodiscard]] auto result() const -> const CoordinationResult & {
        return m_result;
    }

    [[nodiscard]] auto sessionId() const -> QString {
        return m_request.sessionId;
    }

protected:
    void run() override;

private:
    ReqId m_req_id;
    Operation m_operation;
    plugin::SigningRequest m_request;
    QString m_endpoint;
    QString m_tx_hex;
    CoordinationResult m_result;
};
