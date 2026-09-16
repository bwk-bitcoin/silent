#include "SdPicker.h"

#include "catalog/Button.h"
#include "catalog/feedback/ModalStatus.h"
#include "i18n/Tr.h"
#include "views/utils.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>

#include <common.h>

using catalog::Button;
using catalog::ButtonRole;
using catalog::ModalStatus;

namespace keys::modal {

SdPicker::SdPicker(const QString &key_alias, QWidget *parent) : m_key_alias(key_alias) {
    Q_UNUSED(parent);
    setWindowTitle(m_key_alias);
    init();
    doConnect();
    updateStep();
    view();
}

void SdPicker::init() {
    m_status = new ModalStatus;
    m_choose_btn = new Button(TR("keys-sd-save-title"), ButtonRole::Primary);
    m_cancel_btn = new Button(TR("common-cancel"));
}

void SdPicker::doConnect() {
    connect(m_choose_btn, &QPushButton::clicked, this, &SdPicker::onChooseClicked, qontrol::UNIQUE);
    connect(m_cancel_btn, &QPushButton::clicked, this, &SdPicker::onCancelClicked, qontrol::UNIQUE);
}

void SdPicker::view() {
    auto *btnRow = (new qontrol::Row)
                       ->pushSpacer()
                       ->push(m_cancel_btn)
                       ->pushSpacer(resolve(Spacing::XS))
                       ->push(m_choose_btn)
                       ->pushSpacer();
    auto *col = (new qontrol::Column)
                    ->push(m_status)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push(btnRow)
                    ->pushSpacer();
    setMainWidget(margin(col));
}

void SdPicker::updateStep() {
    switch (m_step) {
    case Step::Save:
        m_status->setState(ModalStatus::State::Review);
        m_status->setSubtitle(TR("confirm-send-sd-step"));
        m_choose_btn->setText(TR("keys-sd-save-title"));
        break;
    case Step::Load:
        m_status->setState(ModalStatus::State::Review);
        m_status->setSubtitle(TR("keys-sd-load-step"));
        m_choose_btn->setText(TR("keys-sd-load-title"));
        break;
    }
}

void SdPicker::onChooseClicked() {
    switch (m_step) {
    case Step::Save: {
        QString path = QFileDialog::getSaveFileName(this, TR("keys-sd-save-title"),
                                                    QDir::homePath(), TR("keys-sd-filter"));
        if (path.isEmpty()) {
            return;
        }
        QFileInfo info(path);
        if (info.suffix().isEmpty()) {
            path += QStringLiteral(".psbt");
        }
        emit saveRequested(path);
        break;
    }
    case Step::Load: {
        QString path = QFileDialog::getOpenFileName(this, TR("keys-sd-load-title"),
                                                    QDir::homePath(), TR("keys-sd-filter"));
        if (path.isEmpty()) {
            return;
        }
        emit loadRequested(path);
        break;
    }
    }
}

void SdPicker::onCancelClicked() {
    emit cancelled();
    reject();
}

void SdPicker::onSaveDone(bool is_ok, const QString &error) {
    if (!is_ok) {
        m_status->setState(ModalStatus::State::Error);
        m_status->setSubtitle(TR("keys-sd-save-failed") + "\n\n" + error);
        return;
    }
    m_step = Step::Load;
    updateStep();
}

void SdPicker::onLoadDone(bool is_ok, const QString &error) {
    if (!is_ok) {
        m_status->setState(ModalStatus::State::Error);
        m_status->setSubtitle(TR("keys-sd-load-failed") + "\n\n" + error);
        return;
    }
    accept();
}

} // namespace keys::modal
