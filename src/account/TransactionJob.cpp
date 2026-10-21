#include "TransactionJob.h"

#include "account/Spending.h"
#include <QDebug>
#include <utility>

TransactionJob::TransactionJob(ReqId req_id, Operation operation, plugin::SigningRequest request,
                               QString endpoint, QString tx_hex)
    : m_req_id(req_id),
      m_operation(operation),
      m_request(std::move(request)),
      m_endpoint(std::move(endpoint)),
      m_tx_hex(std::move(tx_hex)) {
}

void TransactionJob::run() {
    if (!account::validRequest(m_request)) {
        m_result.is_ok = false;
        m_result.kind = SpendError::InvalidRequest;
        m_result.error = "invalid public enum value";
        return;
    }
    auto network = *account::toBridge(m_request.network);
    auto psbt = m_request.psbtV2Hex.toStdString();
    switch (m_operation) {
    case Operation::Finalize:
        m_result = transaction_finalize(network, psbt, account::selectedInputs(m_request.inputs),
                                        m_request.sessionId.toStdString());
        break;
    case Operation::Broadcast:
        m_result = transaction_broadcast(network, m_endpoint.toStdString(), m_tx_hex.toStdString());
        break;
    case Operation::Release:
        m_result = coordination_release(network, psbt, account::selectedInputs(m_request.inputs),
                                        m_request.sessionId.toStdString());
        if (!m_result.is_ok) {
            qWarning() << "MuSig retirement failed:"
                       << QString::fromStdString(std::string(m_result.error));
        }
        break;
    }
}
