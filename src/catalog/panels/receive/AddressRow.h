#pragma once

#include <QWidget>

namespace catalog {

class Button;
class ChunkedAddress;
class CopyButton;
class Label;
class LabelRow;
class QrCode;

class AddressRow : public QWidget {
    Q_OBJECT

public:
    explicit AddressRow(QWidget *parent = nullptr);
    void setAddress(const QString &address);
    [[nodiscard]] auto address() const -> QString;
    // When true, a long address is split across two centered lines (silent
    // payment); otherwise it stays on a single line.
    void setTwoLineAddress(bool two_lines);
    void setCopyText(const QString &text);
    void setGenerateText(const QString &text);
    void setGenerateVisible(bool visible);
    void setPlaceholderText(const QString &text);
    void setLabelText(const QString &text);
    [[nodiscard]] auto labelText() const -> QString;
    void setLabelVisible(bool visible);
    void setHistoryText(const QString &text);
    void setHistoryVisible(bool visible);
    void setVerifyText(const QString &text);
    void setVerifyVisible(bool visible);
    // A muted note under the address saying why it cannot be verified. Empty hides it.
    void setVerifyNote(const QString &note);

signals:
    void copyRequested(const QString &address);
    void generateRequested();
    void historyRequested();
    void verifyRequested();

public slots:
    void onCopyClicked();
    void onGenerateClicked();
    void onHistoryClicked();
    void onVerifyClicked();

protected:
    // Show/hide the QR, address row and empty state per the current address
    // (mirrors the design: no address -> only placeholder + Generate).
    void applyVisibility();

private:
    QString m_address_text;
    bool m_label_enabled = false;
    bool m_history_enabled = false;
    ChunkedAddress *m_address = nullptr;
    QrCode *m_qr_code = nullptr;
    LabelRow *m_label = nullptr;
    QWidget *m_empty_state = nullptr;
    QWidget *m_address_row = nullptr;
    Label *m_placeholder = nullptr;
    CopyButton *m_copy = nullptr;
    Button *m_generate = nullptr;
    Button *m_history = nullptr;
    Button *m_verify = nullptr;
    QWidget *m_verify_note = nullptr;
    Label *m_verify_note_text = nullptr;
};

} // namespace catalog
