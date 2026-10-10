#include "FetchXpub.h"

#include "FetchXpubStep.h"

#include "modules/keys/DeviceName.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/KeysView.h"

#include "catalog/feedback/ModalStatus.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "theme/Palette.h"

#include <QDebug>

#include <common.h>

namespace keys::modal {

FetchXpub::FetchXpub(KeysModule *module, const QString &fingerprint)
    : m_module(module),
      m_fingerprint(fingerprint) {
    init();
    doConnect();
    view();
}

void FetchXpub::init() {
    m_fetch = new FetchXpubStep;
    QStringList cached;
    for (const auto &entry :
         m_module->rust()->key_xpubs(rust::String(m_fingerprint.toStdString()))) {
        cached.append(xpubPath(KeysModule::toQt(entry.path)));
    }
    m_fetch->reset(cached);
    m_fetching = new catalog::ModalStatus;
    m_fetching->setSpinner();
    m_fetching->setTitle(TR("keys-fetching-title"));
    m_fetching->setPadding(resolve(Spacing::M), resolve(Spacing::M));
}

void FetchXpub::doConnect() {
    connect(m_fetch, &FetchXpubStep::fetchClicked, this, &FetchXpub::onFetchClicked,
            qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &FetchXpub::onInstanceRemoved, qontrol::UNIQUE);
}

void FetchXpub::view() {
    m_fetch_page = addStep(TR("keys-fetch-title"), ModalWidth::L, m_fetch, m_fetch->actions());
    m_fetching_page = addStep(TR("keys-fetch-title"), ModalWidth::L, m_fetching, nullptr);
    showStep(m_fetch_page);
}

void FetchXpub::onFetchClicked(const QString &path) {
    auto device = readyDevice(m_fingerprint);
    if (!device.has_value()) {
        showError(TR("keys-fetch-xpub-connect"));
        return;
    }
    m_signer = device->signer;
    m_instance = device->instanceId;
    m_path = path;
    connect(device->signer, &ISigner::xpub, this, &FetchXpub::onDeviceXpub, qontrol::UNIQUE);
    connect(device->signer, &ISigner::error, this, &FetchXpub::onDeviceError, qontrol::UNIQUE);
    m_req = device->signer->getXpub(path);
    m_fetching->setSubtitle(TR("keys-fetching-sub").arg(deviceName(*device->signer)));
    showStep(m_fetching_page);
}

void FetchXpub::onDeviceXpub(ReqId req_id, const QString &xpub) {
    if (m_req == 0 || req_id != m_req) {
        return;
    }
    QString instance = m_instance;
    QString path = m_path;
    stopFetch();
    auto result = m_module->cacheXpub(m_fingerprint, path, xpub);
    if (!result.is_ok) {
        showError(KeysModule::toQt(result.error));
        return;
    }
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && entry.instanceId == instance) {
            auto recorded = m_module->recordDevice(entry);
            if (!recorded.is_ok) {
                qWarning() << "keys:" << KeysModule::toQt(recorded.error);
            }
        }
    }
    accept();
}

void FetchXpub::onDeviceError(std::optional<ReqId> req_id, const QString &message) {
    if (m_req == 0 || !req_id.has_value() || req_id.value() != m_req) {
        return;
    }
    stopFetch();
    showError(message);
}

void FetchXpub::onInstanceRemoved(const QString &id) {
    if (m_req == 0 || id != m_instance) {
        return;
    }
    stopFetch();
    showError(TR("keys-register-disconnected"));
}

void FetchXpub::stopFetch() {
    // Only this fetch's own connections: the device keeps serving the key detail.
    if (!m_signer.isNull()) {
        disconnect(m_signer, &ISigner::xpub, this, &FetchXpub::onDeviceXpub);
        disconnect(m_signer, &ISigner::error, this, &FetchXpub::onDeviceError);
    }
    m_signer = nullptr;
    m_req = 0;
}

void FetchXpub::showError(const QString &error) {
    m_fetch->showError(error);
    // Shown again so the step fits the error it now holds.
    showStep(m_fetch_page);
}

} // namespace keys::modal
