#include "account/PsbtTransfer.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QPushButton>
#include <QRegularExpression>
#include <Qontrol>
#include <common.h>

namespace account {

namespace {

const char *const PSBT_SUFFIX = ".psbt";

} // namespace

auto writePsbtFile(const QString &path, const QString &base64) -> QString {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(base64.toLatin1()) != base64.size()) {
        return file.errorString();
    }
    return {};
}

auto psbtFileName(const QString &name) -> QString {
    static const QRegularExpression c_unsafe(QStringLiteral("[^A-Za-z0-9._-]+"));
    auto base = QString(name).replace(c_unsafe, QStringLiteral("_"));
    if (base.isEmpty()) {
        base = QStringLiteral("psbt-") +
               QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    }
    return base + PSBT_SUFFIX;
}

ExportPsbt::ExportPsbt(const QString &base64, const QString &name)
    : m_base64(base64),
      m_name(name) {
    init();
    doConnect();
    view();
}

void ExportPsbt::init() {
    m_message_label = new catalog::Label(TR("psbt-export-message"));
    m_message_label->setWordWrap(true);
    m_notice = new catalog::Notice(catalog::Notice::Tone::Success);
    m_notice->hide();
    m_save_btn = catalog::btnSaveToFile();
    m_copy_btn = catalog::btnCopyToClipboard();
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_save_btn);
    m_actions->addAction(m_copy_btn);
}

void ExportPsbt::doConnect() {
    connect(m_save_btn, &QPushButton::clicked, this, &ExportPsbt::onSaveClicked, qontrol::UNIQUE);
    connect(m_copy_btn, &QPushButton::clicked, this, &ExportPsbt::onCopyClicked, qontrol::UNIQUE);
}

void ExportPsbt::view() {
    setWindowTitle(TR("psbt-export-title"));
    auto *body =
        (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_message_label)->push(m_notice);
    setBody(body);
    setFooter(m_actions);
}

void ExportPsbt::onSaveClicked() {
    auto path = QFileDialog::getSaveFileName(this, TR("psbt-export-save-title"),
                                             QDir::home().filePath(psbtFileName(m_name)),
                                             TR("keys-sd-filter"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += PSBT_SUFFIX;
    }
    auto error = writePsbtFile(path, m_base64);
    m_notice->setTone(error.isEmpty() ? catalog::Notice::Tone::Success
                                      : catalog::Notice::Tone::Error);
    m_notice->setText(error.isEmpty() ? TR("psbt-export-saved").arg(QDir::toNativeSeparators(path))
                                      : TR("psbt-export-save-failed").arg(error));
    m_notice->show();
    refit();
}

void ExportPsbt::onCopyClicked() {
    QApplication::clipboard()->setText(m_base64);
    m_notice->setTone(catalog::Notice::Tone::Success);
    m_notice->setText(TR("psbt-export-copied"));
    m_notice->show();
    refit();
}

ImportPsbt::ImportPsbt() {
    init();
    doConnect();
    view();
}

void ImportPsbt::init() {
    m_message_label = new catalog::Label(TR("psbt-import-message"));
    m_message_label->setWordWrap(true);
    m_notice = new catalog::Notice(catalog::Notice::Tone::Error);
    m_notice->hide();
    m_open_btn = catalog::btnOpenFile();
    m_paste_btn = catalog::btnPasteFromClipboard();
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_paste_btn);
    m_actions->addAction(m_open_btn);
}

void ImportPsbt::doConnect() {
    connect(m_open_btn, &QPushButton::clicked, this, &ImportPsbt::onOpenClicked, qontrol::UNIQUE);
    connect(m_paste_btn, &QPushButton::clicked, this, &ImportPsbt::onPasteClicked, qontrol::UNIQUE);
}

void ImportPsbt::view() {
    setWindowTitle(TR("psbt-import-title"));
    auto *body =
        (new qontrol::Column)->spacing(resolve(Spacing::M))->push(m_message_label)->push(m_notice);
    setBody(body);
    setFooter(m_actions);
}

void ImportPsbt::showError(const QString &message) {
    m_notice->setTone(catalog::Notice::Tone::Error);
    m_notice->setText(TR("psbt-import-failed").arg(message));
    m_notice->show();
    refit();
}

void ImportPsbt::showImported(const QString &message) {
    m_notice->setTone(catalog::Notice::Tone::Success);
    m_notice->setText(message);
    m_notice->show();
    refit();
}

void ImportPsbt::onOpenClicked() {
    auto path = QFileDialog::getOpenFileName(this, TR("psbt-import-open-title"), QDir::homePath(),
                                             TR("keys-sd-filter"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        showError(file.errorString());
        return;
    }
    emit textChosen(file.readAll());
}

void ImportPsbt::onPasteClicked() {
    auto text = QApplication::clipboard()->text().trimmed();
    if (text.isEmpty()) {
        showError(TR("psbt-import-clipboard-empty"));
        return;
    }
    emit textChosen(text.toUtf8());
}

} // namespace account
