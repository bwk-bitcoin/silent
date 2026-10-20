#include "HotSign.h"

#include "FlowCopy.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/modals/Unseal.h"

#include <Qontrol>
#include <common.h>

#include <utility>

namespace keys::flow {

HotSign::HotSign(KeysModule *module, ReqId bus_req, Network network, QString fingerprint,
                 QString alias, QString psbt)
    : QObject(module),
      m_module(module),
      m_bus_req(bus_req),
      m_network(network),
      m_fingerprint(std::move(fingerprint)),
      m_alias(std::move(alias)),
      m_psbt(std::move(psbt)) {
    auto queued = static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection);
    connect(module, &KeysModule::hotSigned, this, &HotSign::onHotSigned, queued);
    connect(module, &KeysModule::hotCachedReady, this, &HotSign::onCachedReady, qontrol::UNIQUE);
    connect(module->bus(), &IKeys::requestCancelled, this, &HotSign::onRequestCancelled,
            qontrol::UNIQUE);
}

void HotSign::start() {
    m_hot_req = m_module->nextReqId();
    NetworkScope scope(m_module, m_network);
    m_module->publishHotCached(m_hot_req, m_fingerprint, m_psbt);
}

void HotSign::askPassword() {
    if (!m_live) {
        return;
    }
    auto copy = flowCopy(Context::Sign);
    modal::Unseal prompt(copy.title, copy.hotPrompt.arg(m_alias), hotProceedButton(Context::Sign));
    prompt.setCloseOnSubmit(true);
    if (m_wrong_password) {
        prompt.showError(TR("flow-password-wrong"));
        m_wrong_password = false;
    }
    m_submitted = false;
    connect(&prompt, &modal::Unseal::passwordEntered, this, &HotSign::onPasswordEntered,
            qontrol::UNIQUE);
    m_prompt = &prompt;
    prompt.exec();
    m_prompt.clear();
    if (!m_live) {
        emit finished();
        return;
    }
    if (!m_submitted) {
        m_live = false;
        emit cancelled(m_bus_req);
        emit finished();
    }
}

void HotSign::onPasswordEntered(const QString &password) {
    if (!m_live) {
        return;
    }
    m_submitted = true;
    m_hot_req = m_module->nextReqId();
    NetworkScope scope(m_module, m_network);
    m_module->signHot(m_hot_req, m_fingerprint, password, m_psbt);
}

void HotSign::onHotSigned(ReqId req_id, bool is_ok, const QString &psbt_v2_hex,
                          const QString &error, bool wrong_password, bool musig_session) {
    if (!m_live || req_id == 0 || req_id != m_hot_req) {
        return;
    }
    m_hot_req = 0;
    if (wrong_password) {
        m_wrong_password = true;
        askPassword();
        return;
    }
    m_live = false;
    if (is_ok) {
        emit signedPsbt(m_bus_req, psbt_v2_hex);
    } else {
        emit failed(m_bus_req, error,
                    musig_session ? plugin::SignError::MusigSession : plugin::SignError::Other);
    }
    emit finished();
}

void HotSign::onCachedReady(ReqId req_id, bool needs_password, bool is_ok,
                           const QString &psbt_v2_hex, const QString &error, bool musig_session) {
    if (!m_live || m_hot_req == 0 || req_id != m_hot_req) {
        return;
    }
    if (needs_password && is_ok) {
        m_hot_req = 0;
        askPassword();
        return;
    }
    onHotSigned(req_id, is_ok, psbt_v2_hex, error, false, musig_session);
}

void HotSign::onRequestCancelled(ReqId req_id) {
    if (!m_live || req_id != m_bus_req) {
        return;
    }
    m_live = false;
    m_hot_req = 0;
    if (!m_prompt.isNull()) {
        m_prompt->reject();
        return;
    }
    emit finished();
}

} // namespace keys::flow
