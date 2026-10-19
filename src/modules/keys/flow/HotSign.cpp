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
}

void HotSign::start() {
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
    prompt.exec();
    if (!m_submitted) {
        emit cancelled(m_bus_req);
        emit finished();
    }
}

void HotSign::onPasswordEntered(const QString &password) {
    m_submitted = true;
    m_hot_req = m_module->nextReqId();
    NetworkScope scope(m_module, m_network);
    m_module->signHot(m_hot_req, m_fingerprint, password, m_psbt);
}

void HotSign::onHotSigned(ReqId req_id, bool is_ok, const QString &psbt_v2_hex,
                          const QString &error, bool wrong_password, bool musig_session) {
    if (req_id == 0 || req_id != m_hot_req) {
        return;
    }
    m_hot_req = 0;
    if (wrong_password) {
        m_wrong_password = true;
        start();
        return;
    }
    if (is_ok) {
        emit signedPsbt(m_bus_req, psbt_v2_hex);
    } else {
        emit failed(m_bus_req, error,
                    musig_session ? plugin::SignError::MusigSession : plugin::SignError::Other);
    }
    emit finished();
}

} // namespace keys::flow
