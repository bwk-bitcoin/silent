#include "ScanAddress.h"

#include "catalog/containers/ModalActions.h"
#include "i18n/Tr.h"
#include "modules/keys/flow/CameraStep.h"
#include "qr/QrCameraEngine.h"
#include "qr/QrTypes.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace modal {

ScanAddress::ScanAddress() {
    setWindowTitle(TR("send-scan-address-title"));
    init();
    doConnect();
    view();
    m_camera->start();
}

ScanAddress::~ScanAddress() {
    m_camera->stop();
}

auto ScanAddress::text() const -> QString {
    return m_text;
}

void ScanAddress::init() {
    m_camera = new keys::flow::CameraStep(nullptr, qr::PlainCodes::Read);
    m_camera->setHint(TR("send-scan-address-hint"));
}

void ScanAddress::doConnect() {
    connect(m_camera->engine(), &qr::QrCameraEngine::response, this, &ScanAddress::onResponse,
            qontrol::UNIQUE);
    connect(m_camera, &keys::flow::CameraStep::backClicked, this, &QDialog::reject,
            qontrol::UNIQUE);
}

void ScanAddress::view() {
    setBody(m_camera);
    setFooter(m_camera->actions());
    setWidth(ModalWidth::L);
}

void ScanAddress::onResponse(const qr::QrResponse &response) {
    // The first code read is the address: the input checks it.
    if (!m_text.isEmpty()) {
        return;
    }
    m_text = QString::fromUtf8(response.payload).trimmed();
    m_camera->stop();
    accept();
}

} // namespace modal
