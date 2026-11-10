#include "modules/multikey/MusigServices.h"

#include "account/RequestId.h"
#include "interfaces/backend/blockchain.h"

#include <QDebug>
#include <QThread>

namespace musig {

namespace {

auto toRust(const QString &value) -> rust::String {
    return rust::String(value.toStdString());
}

auto toRustBytes(const QByteArray &bytes) -> rust::Vec<std::uint8_t> {
    rust::Vec<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(bytes.size()));
    for (auto byte : bytes) {
        out.push_back(static_cast<std::uint8_t>(byte));
    }
    return out;
}

// What `chain`, watching with gap limit `gap_limit`, reports, as the account reads it.
auto chainView(const backend::IBlockchain &chain, quint32 gap_limit) -> ChainView {
    ChainView view{};
    for (const auto &output : chain.ownedOutputs()) {
        view.outputs.push_back(ChainOutput{.outpoint = toRust(output.outpoint),
                                           .amount = output.amount,
                                           .script_pubkey = toRustBytes(output.scriptPubKey),
                                           .descriptor = output.descriptor,
                                           .keychain = output.keychain,
                                           .index = output.index,
                                           .height = output.height.value_or(0),
                                           .coinbase = output.coinbase,
                                           .spent_by = toRust(output.spentBy)});
    }
    for (const auto &used : chain.usedIndexes()) {
        view.used.push_back(ChainUsedIndex{.descriptor = used.descriptor,
                                           .keychain = used.keychain,
                                           .used = used.last.has_value(),
                                           .last = used.last.value_or(0)});
    }
    auto tip = chain.state().tip;
    view.gap_limit = gap_limit;
    view.tip = tip.value_or(0);
    view.has_tip = tip.has_value();
    return view;
}

} // namespace

MusigServices::MusigServices(const QString &account, MusigAccountController *controller)
    : AccountServices(account, controller) {
}

void MusigServices::broadcast(backend::RequestId token, QByteArray transaction) {
    if (!available() || !owner()->startBroadcast(transaction, [this, token](const auto &result) {
            onBroadcastDone(token, result);
        })) {
        emit failed(token, busy());
    }
}

void MusigServices::newAddress(backend::RequestId token, QString kind) {
    if (m_chain == nullptr) {
        AccountServices::newAddress(token, kind);
        return;
    }
    auto *wallet = rustAccount();
    if (wallet == nullptr) {
        emit failed(token, busy());
        return;
    }
    auto next = wallet->next_chain_receive(rust::Str(kind.toStdString()));
    if (!next.is_ok) {
        emit failed(token, account::services::toQString(next.error));
        return;
    }
    if (!next.reveal) {
        takeReceive(token, next.index);
        return;
    }
    auto request = account::nextRequestId();
    // Registered before the request: the answer may come during the call.
    m_reveals.insert(request, {.token = token, .index = next.index});
    m_chain->reveal(request, next.descriptor, next.keychain, next.index);
}

void MusigServices::attachChain(backend::IBlockchain *chain, quint32 gap_limit) {
    if (m_chain != nullptr) {
        disconnect(m_chain.data(), nullptr, this, nullptr);
    }
    auto reveals = std::move(m_reveals);
    m_reveals.clear();
    for (const auto &reveal : reveals) {
        emit failed(reveal.token, QStringLiteral("The blockchain backend changed"));
    }
    m_chain = chain;
    m_gap_limit = gap_limit;
    if (chain != nullptr) {
        connect(chain, &backend::IBlockchain::coinsChanged, this, &MusigServices::onChainChanged,
                qontrol::UNIQUE);
        connect(chain, &backend::IBlockchain::stateChanged, this, &MusigServices::onChainChanged,
                qontrol::UNIQUE);
        connect(chain, &backend::IBlockchain::completed, this, &MusigServices::onChainCompleted,
                qontrol::UNIQUE);
        connect(chain, &backend::IService::error, this, &MusigServices::onChainError,
                qontrol::UNIQUE);
        connect(chain, &backend::IBlockchain::broadcastFinished, this,
                &MusigServices::onChainBroadcastFinished, qontrol::UNIQUE);
    }
    feedChain();
}

void MusigServices::feedChain() {
    auto *wallet = rustAccount();
    if (wallet == nullptr) {
        return;
    }
    if (m_chain == nullptr) {
        wallet->clear_chain_view();
        return;
    }
    if (!wallet->set_chain_view(chainView(*m_chain, m_gap_limit))) {
        qWarning() << "MusigServices: the account refused what its blockchain backend reports";
    }
}

void MusigServices::takeReceive(backend::RequestId token, quint32 index) {
    auto *wallet = rustAccount();
    if (wallet == nullptr) {
        emit failed(token, busy());
        return;
    }
    startWork(token, QThread::create([this, wallet, token, index] {
                  auto result = wallet->take_chain_receive(index);
                  emit addressFinished(token, result.is_ok,
                                       account::services::toAddress(result.address),
                                       account::services::toQString(result.error));
              }));
}

// ===== Slots =====

void MusigServices::onChainChanged() {
    feedChain();
}

void MusigServices::onChainCompleted(backend::RequestId request_id) {
    if (!m_reveals.contains(request_id)) {
        return;
    }
    auto reveal = m_reveals.take(request_id);
    takeReceive(reveal.token, reveal.index);
}

void MusigServices::onChainError(backend::RequestId request_id, const QString &message) {
    if (!m_reveals.contains(request_id)) {
        return;
    }
    emit failed(m_reveals.take(request_id).token, message);
}

void MusigServices::onChainBroadcastFinished([[maybe_unused]] backend::RequestId request_id,
                                             const backend::BroadcastResult &result) {
    if (result.outcome == backend::BroadcastOutcome::Accepted) {
        owner()->recordBroadcast(result.txid);
    }
}

} // namespace musig
