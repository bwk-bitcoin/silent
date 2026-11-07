#pragma once

#include "catalog/containers/Modal.h"

#include <QByteArray>
#include <QString>

namespace catalog {
class Button;
class Label;
class ModalActions;
class Notice;
} // namespace catalog

namespace account {

// Writes `base64`, a PSBT as text, to the file `path`, replacing it. The error, empty
// once written.
auto writePsbtFile(const QString &path, const QString &base64) -> QString;
// The default file name of an exported PSBT named `name` (a txid or a label): unsafe
// characters replaced, ".psbt" appended. A timestamp when `name` is empty.
auto psbtFileName(const QString &name) -> QString;

// Export of a PSBT as base64 text: Save to File writes it to a .psbt file, Copy to
// Clipboard copies it. Each says it is done in the modal.
class ExportPsbt final : public catalog::Modal {
    Q_OBJECT

public:
    ExportPsbt(const QString &base64, const QString &name);

public slots:
    void onSaveClicked();
    void onCopyClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    QString m_base64;
    QString m_name;
    catalog::Label *m_message_label = nullptr;
    catalog::Notice *m_notice = nullptr;
    catalog::Button *m_save_btn = nullptr;
    catalog::Button *m_copy_btn = nullptr;
    catalog::ModalActions *m_actions = nullptr;
};

// Import of a PSBT as base64 text, from a .psbt file or the clipboard: hands what it read
// to textChosen(), whose receiver says how it went with showError() or showImported().
class ImportPsbt final : public catalog::Modal {
    Q_OBJECT

public:
    ImportPsbt();
    void showError(const QString &message);
    void showImported(const QString &message);

signals:
    void textChosen(QByteArray text);

public slots:
    void onOpenClicked();
    void onPasteClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    catalog::Label *m_message_label = nullptr;
    catalog::Notice *m_notice = nullptr;
    catalog::Button *m_open_btn = nullptr;
    catalog::Button *m_paste_btn = nullptr;
    catalog::ModalActions *m_actions = nullptr;
};

} // namespace account
