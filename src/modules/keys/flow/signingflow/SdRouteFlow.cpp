#include "SdRouteFlow.h"

#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Badge.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "modules/keys/KeysModule.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <Qontrol>

#include <utility>

#include <common.h>

namespace keys::flow {

namespace {

const QString UNSIGNED_SUFFIX = QStringLiteral("-unsigned.psbt");
const QString SIGNED_SUFFIX = QStringLiteral("-signed.psbt");
const QString PSBT_EXTENSION = QStringLiteral(".psbt");

// A file name inside a sentence, set in the mono face.
auto monoFileName(const QString &name) -> QString {
    return QStringLiteral("<span style=\"font-family: '%1'\">%2</span>")
        .arg(font::MONO, name.toHtmlEscaped());
}

} // namespace

SdRouteFlow::SdRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row,
                         QString psbt)
    : SigningRouteFlow(module, std::move(request), std::move(row), std::move(psbt)) {
    init();
    doConnect();
    view();
}

void SdRouteFlow::start() {
    m_sd_phase = SdPhase::Save;
    m_sd_failure = SdFailure::None;
    showSd();
}

void SdRouteFlow::init() {
    initSd();
}

void SdRouteFlow::doConnect() {
    connectSd();
}

void SdRouteFlow::view() {
    m_sd_step = viewSd();
}

// ===== SD card view =====

void SdRouteFlow::initSd() {
    m_sd_save_row = new catalog::SelectRow;
    m_sd_save_row->setTitle(TR("flow-sd-save-title"));
    m_sd_save_row->setSub(
        TR("flow-sd-save-body").arg(monoFileName(request().wallet_name + UNSIGNED_SUFFIX)));
    m_sd_save_row->setClickable(false);
    m_sd_load_row = new catalog::SelectRow;
    m_sd_load_row->setTitle(TR("flow-sd-load-title"));
    m_sd_load_row->setSub(
        TR("flow-sd-load-body").arg(monoFileName(request().wallet_name + SIGNED_SUFFIX)));
    m_sd_load_row->setClickable(false);
    m_sd_write_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_sd_write_error->setTitle(TR("flow-sd-write-error-title"));
    m_sd_write_error->setText(TR("flow-sd-write-error-body"));
    m_sd_read_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_sd_read_error->setTitle(TR("flow-sd-read-error-title"));
    m_sd_read_error->setText(TR("flow-sd-read-error-body"));
    m_sd_save = catalog::btnSaveUnsignedPsbt();
    m_sd_save_retry = catalog::btnTryAgain();
    m_sd_load = catalog::btnLoadSignedPsbt();
    m_sd_load_retry = catalog::btnPickAnotherFile();
    m_sd_actions = new catalog::ModalActions;
    m_sd_actions->addAction(m_sd_save);
    m_sd_actions->addAction(m_sd_save_retry);
    m_sd_actions->addAction(m_sd_load);
    m_sd_actions->addAction(m_sd_load_retry);
}

void SdRouteFlow::connectSd() {
    connect(m_sd_save, &QPushButton::clicked, this, &SdRouteFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_sd_save_retry, &QPushButton::clicked, this, &SdRouteFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(m_sd_load, &QPushButton::clicked, this, &SdRouteFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_sd_load_retry, &QPushButton::clicked, this, &SdRouteFlow::onNextClicked,
            qontrol::UNIQUE);
    connect(module(), &KeysModule::sdSaved, this, &SdRouteFlow::onSdSaved, qontrol::UNIQUE);
    connect(module(), &KeysModule::sdLoaded, this, &SdRouteFlow::onSdLoaded, qontrol::UNIQUE);
}

auto SdRouteFlow::viewSd() -> QWidget * {
    return (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_sd_save_row)
        ->push(m_sd_load_row)
        ->push(m_sd_write_error)
        ->push(m_sd_read_error);
}

void SdRouteFlow::showSd() {
    refreshSd();
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_sd_step);
    viewSlots()->setFooter(m_sd_actions);
    emit viewChanged(copy().sdTitle, ModalWidth::M);
}

void SdRouteFlow::refreshSd() {
    bool saving = m_sd_phase == SdPhase::Save;
    m_sd_save_row->setIcon(saving ? icon::sdCard : icon::circleCheck);
    m_sd_save_row->setCurrent(saving);
    m_sd_save_row->setStatus(saving ? nullptr : catalog::pillSdStepDone());
    m_sd_load_row->setIcon(icon::sdCard);
    m_sd_load_row->setCurrent(!saving);
    m_sd_load_row->setEnabled(!saving);

    bool writeFailed = m_sd_failure == SdFailure::Write;
    bool readFailed = m_sd_failure == SdFailure::Read;
    m_sd_write_error->setVisible(writeFailed);
    m_sd_read_error->setVisible(readFailed);
    m_sd_save->setVisible(saving && !writeFailed);
    m_sd_save_retry->setVisible(saving && writeFailed);
    m_sd_load->setVisible(!saving && !readFailed);
    m_sd_load_retry->setVisible(!saving && readFailed);
}

void SdRouteFlow::saveSd() {
    QDir folder(m_sd_dir.isEmpty() ? QDir::homePath() : m_sd_dir);
    QString path = QFileDialog::getSaveFileName(
        this, TR("keys-sd-save-title"), folder.filePath(request().wallet_name + UNSIGNED_SUFFIX),
        TR("keys-sd-filter"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += PSBT_EXTENSION;
    }
    m_sd_dir = QFileInfo(path).absolutePath();
    m_sd_req = module()->nextReqId();
    module()->saveSdPsbt(m_sd_req, path, psbt());
}

void SdRouteFlow::loadSd() {
    QDir folder(m_sd_dir.isEmpty() ? QDir::homePath() : m_sd_dir);
    QString path = QFileDialog::getOpenFileName(
        this, TR("keys-sd-load-title"), folder.filePath(request().wallet_name + SIGNED_SUFFIX),
        TR("keys-sd-filter"));
    if (path.isEmpty()) {
        return;
    }
    m_sd_dir = QFileInfo(path).absolutePath();
    m_sd_req = module()->nextReqId();
    module()->loadSdPsbt(m_sd_req, path);
}

// ===== Slots =====

void SdRouteFlow::onNextClicked() {
    if (m_sd_phase == SdPhase::Save) {
        saveSd();
    } else {
        loadSd();
    }
}

void SdRouteFlow::onSdSaved(ReqId req_id, bool is_ok, const QString &error) {
    if (m_sd_req == 0 || req_id != m_sd_req) {
        return;
    }
    m_sd_req = 0;
    if (!is_ok) {
        qWarning() << "keys: sd save:" << error;
        m_sd_failure = SdFailure::Write;
        showSd();
        return;
    }
    m_sd_phase = SdPhase::Load;
    m_sd_failure = SdFailure::None;
    showSd();
}

void SdRouteFlow::onSdLoaded(ReqId req_id, bool is_ok, const QString &psbt_hex,
                             const QString &error) {
    if (m_sd_req == 0 || req_id != m_sd_req) {
        return;
    }
    m_sd_req = 0;
    // The unsigned file read back, or a file this wallet cannot plan, is not the
    // answer the device wrote.
    bool readable = is_ok && psbt_hex != psbt() &&
                    module()->rust()->signing_plan(rust::String(psbt_hex.toStdString())).is_ok;
    if (!readable) {
        qWarning() << "keys: sd load:" << error;
        m_sd_failure = SdFailure::Read;
        showSd();
        return;
    }
    emit psbtSigned(psbt_hex);
}

} // namespace keys::flow
