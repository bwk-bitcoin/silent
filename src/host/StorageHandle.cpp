#include "StorageHandle.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <silent.h>
#include <utility>

namespace host {

StorageJob::StorageJob(quint64 session, QString id, QByteArray message)
    : m_session(session),
      m_id(std::move(id)),
      m_message(std::move(message)) {
}

void StorageJob::run() {
    // Read first: an answer computed before a grant or key change is fenced on delivery.
    m_generation = ::storage_session_generation(m_session);
    auto answer = ::storage_request(
        m_session, rust::String(m_message.constData(), static_cast<size_t>(m_message.size())));
    m_result = QByteArray(answer.data(), static_cast<qsizetype>(answer.size()));
}

StorageHandle::StorageHandle(quint64 session) : m_session(session) {
}

StorageHandle::~StorageHandle() {
    revoke();
}

void StorageHandle::request(const QByteArray &message) {
    if (message.size() > MAX_MESSAGE_SIZE) {
        refuse({}, QStringLiteral("too_large"));
        return;
    }
    QJsonParseError parseError;
    auto document = QJsonDocument::fromJson(message, &parseError);
    auto id = document.object().value(QStringLiteral("id"));
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || !id.isString()) {
        refuse({}, QStringLiteral("invalid_request"));
        return;
    }
    auto requestId = id.toString();
    if (requestId.isEmpty() || requestId.toUtf8().size() > MAX_ID_SIZE) {
        refuse({}, QStringLiteral("invalid_request"));
        return;
    }
    if (m_revoked) {
        refuse(requestId, QStringLiteral("revoked"));
        return;
    }
    if (outstanding(requestId)) {
        refuse(requestId, QStringLiteral("invalid_request"));
        return;
    }
    auto running = m_job != nullptr ? 1 : 0;
    if (m_queue.size() + running >= MAX_OUTSTANDING) {
        refuse(requestId, QStringLiteral("busy"));
        return;
    }
    // Serialized again, so Rust always gets valid UTF-8.
    m_queue.append({.id = requestId, .message = document.toJson(QJsonDocument::Compact)});
    dispatch();
}

void StorageHandle::cancel(const QString &id) {
    if (m_revoked) {
        return;
    }
    if (m_job != nullptr && !m_job_cancelled && m_job->id() == id) {
        m_job_cancelled = true;
        ::storage_request_cancel(m_session, rust::String(id.toStdString()));
        refuse(id, QStringLiteral("cancelled"));
        return;
    }
    auto queued =
        std::ranges::find_if(m_queue, [&id](const Queued &entry) { return entry.id == id; });
    if (queued != m_queue.end()) {
        m_queue.erase(queued);
        refuse(id, QStringLiteral("cancelled"));
    }
}

void StorageHandle::revoke() {
    if (m_revoked) {
        return;
    }
    m_revoked = true;
    m_queue.clear();
    ::storage_session_revoke(m_session);
}

void StorageHandle::refuse(const QString &id, const QString &error) {
    QJsonObject answer{{QStringLiteral("id"), id}, {QStringLiteral("error"), error}};
    emit completed(QJsonDocument(answer).toJson(QJsonDocument::Compact));
}

auto StorageHandle::outstanding(const QString &id) const -> bool {
    if (m_job != nullptr && m_job->id() == id) {
        return true;
    }
    return std::ranges::any_of(m_queue, [&id](const Queued &entry) { return entry.id == id; });
}

void StorageHandle::dispatch() {
    if (m_job != nullptr || m_queue.empty()) {
        return;
    }
    auto next = m_queue.takeFirst();
    m_job_cancelled = false;
    m_job = new StorageJob(m_session, next.id, next.message);
    connect(m_job, &QThread::finished, this, &StorageHandle::onJobFinished, qontrol::UNIQUE);
    connect(m_job, &QThread::finished, m_job, &QObject::deleteLater, qontrol::UNIQUE);
    m_job->start();
}

void StorageHandle::onJobFinished() {
    auto *job = dynamic_cast<StorageJob *>(sender());
    if (job == nullptr || job != m_job) {
        return;
    }
    m_job = nullptr;
    if (m_revoked) {
        return;
    }
    auto cancelled = m_job_cancelled;
    if (cancelled) {
        // A cancellation that raced the finished worker left Rust waiting for the id.
        ::storage_request_forget(m_session, rust::String(job->id().toStdString()));
    }
    dispatch();
    if (cancelled) {
        return;
    }
    auto error = ::storage_delivery_error(m_session, job->generation());
    if (error.empty()) {
        emit completed(job->result());
        return;
    }
    auto code = QString::fromStdString(std::string(error));
    if (code != QStringLiteral("revoked")) {
        refuse(job->id(), code);
    }
}

} // namespace host
