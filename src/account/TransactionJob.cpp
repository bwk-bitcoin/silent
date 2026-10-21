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
    auto psbt = m_request.psbtV2Hex.toStdString();
    switch (m_operation) {
    case Operation::Finalize:
        m_result =
            transaction_finalize(m_request.network, psbt, account::selectedInputs(m_request.inputs),
                                 m_request.sessionId.toStdString());
        break;
    case Operation::Broadcast:
        m_result = transaction_broadcast(m_request.network, m_endpoint.toStdString(),
                                         m_tx_hex.toStdString());
        break;
    case Operation::Release:
        m_result =
            coordination_release(m_request.network, psbt, account::selectedInputs(m_request.inputs),
                                 m_request.sessionId.toStdString());
        if (!m_result.is_ok) {
            qWarning() << "MuSig retirement failed:"
                       << QString::fromStdString(std::string(m_result.error));
        }
        break;
    }
}
