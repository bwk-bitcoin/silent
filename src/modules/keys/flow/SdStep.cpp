#include "SdStep.h"

#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Badge.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace keys::flow {

namespace {

// A file name inside a sentence, set in the mono face.
auto monoFileName(const QString &name) -> QString {
    return QStringLiteral("<span style=\"font-family: '%1'\">%2</span>")
        .arg(font::MONO, name.toHtmlEscaped());
}

} // namespace

SdStep::SdStep(QWidget *parent) : QWidget(parent) {
    init();
    doConnect();
    view();
    refresh();
}

void SdStep::init() {
    m_save_row = new catalog::SelectRow;
    m_save_row->setTitle(TR("flow-sd-save-title"));
    m_save_row->setClickable(false);
    m_load_row = new catalog::SelectRow;
    m_load_row->setTitle(TR("flow-sd-load-title"));
    m_load_row->setClickable(false);

    m_write_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_write_error->setTitle(TR("flow-sd-write-error-title"));
    m_write_error->setText(TR("flow-sd-write-error-body"));
    m_read_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_read_error->setTitle(TR("flow-sd-read-error-title"));
    m_read_error->setText(TR("flow-sd-read-error-body"));

    m_save = catalog::btnSaveUnsignedPsbt();
    m_save_retry = catalog::btnTryAgain();
    m_load = catalog::btnLoadSignedPsbt();
    m_load_retry = catalog::btnPickAnotherFile();
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_save);
    m_actions->addAction(m_save_retry);
    m_actions->addAction(m_load);
    m_actions->addAction(m_load_retry);
}

void SdStep::doConnect() {
    connect(m_save, &QPushButton::clicked, this, &SdStep::saveClicked, qontrol::UNIQUE);
    connect(m_save_retry, &QPushButton::clicked, this, &SdStep::saveClicked, qontrol::UNIQUE);
    connect(m_load, &QPushButton::clicked, this, &SdStep::loadClicked, qontrol::UNIQUE);
    connect(m_load_retry, &QPushButton::clicked, this, &SdStep::loadClicked, qontrol::UNIQUE);
}

void SdStep::view() {
    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->push(m_save_row)
        ->push(m_load_row)
        ->push(m_write_error)
        ->push(m_read_error)
        ->into(this);
}

void SdStep::setFileBase(const QString &file_base) {
    m_file_base = file_base;
    m_save_row->setSub(
        TR("flow-sd-save-body").arg(monoFileName(m_file_base + QStringLiteral("-unsigned.psbt"))));
    m_load_row->setSub(
        TR("flow-sd-load-body").arg(monoFileName(m_file_base + QStringLiteral("-signed.psbt"))));
}

void SdStep::setPhase(Phase phase) {
    m_phase = phase;
    m_failure = Failure::None;
    refresh();
}

void SdStep::showWriteError() {
    m_failure = Failure::Write;
    refresh();
}

void SdStep::showReadError() {
    m_failure = Failure::Read;
    refresh();
}

auto SdStep::actions() const -> catalog::ModalActions * {
    return m_actions;
}

void SdStep::refresh() {
    bool saving = m_phase == Phase::Save;
    m_save_row->setIcon(saving ? icon::sdCard : icon::circleCheck);
    m_save_row->setCurrent(saving);
    m_save_row->setStatus(saving ? nullptr : catalog::pillSdStepDone());
    m_load_row->setIcon(icon::sdCard);
    m_load_row->setCurrent(!saving);
    m_load_row->setEnabled(!saving);

    bool writeFailed = m_failure == Failure::Write;
    bool readFailed = m_failure == Failure::Read;
    m_write_error->setVisible(writeFailed);
    m_read_error->setVisible(readFailed);
    m_save->setVisible(saving && !writeFailed);
    m_save_retry->setVisible(saving && writeFailed);
    m_load->setVisible(!saving && !readFailed);
    m_load_retry->setVisible(!saving && readFailed);
}

} // namespace keys::flow
