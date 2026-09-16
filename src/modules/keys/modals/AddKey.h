#pragma once

#include <QList>
#include <QString>

#include <Qontrol>
#include <silent.h>

namespace catalog {
class Button;
class Input;
class Label;
class SelectRow;
class TextEdit;
class ValidatedInput;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::modal {

// Registers a key no connected device reports: an airgap key reached over QR or an SD
// card, or a key imported as an origin plus an xpub, or as a file exported by another
// Silent instance. Every path validates the origin and takes the fingerprint from it.
class AddKey final : public qontrol::Modal {
    Q_OBJECT

public:
    enum class Mode { AirgapQr, AirgapSd, ImportXpub, ImportFile, HotKey };

    explicit AddKey(KeysModule *module, QWidget *parent = nullptr);
    ~AddKey() override = default;

signals:
    void keyAdded(QString fingerprint);

public slots:
    void onModeClicked();
    void onOriginChanged();
    void onXpubChanged();
    void onDecodeQrClicked();
    void onBrowseFileClicked();
    void onRegisterClicked();
    void onHotKeyAdded(const QString &fingerprint);

protected:
    void init();
    void doConnect();
    void view();

    void applyMode();
    void updateRegisterState();
    void registerFromFile();
    void showError(const QString &message);
    [[nodiscard]] auto fingerprintFromOrigin() const -> QString;
    [[nodiscard]] auto originIsValid() const -> bool;
    [[nodiscard]] auto pathFromOrigin() const -> QString;

private:
    KeysModule *m_module = nullptr;
    Mode m_mode = Mode::AirgapQr;
    QList<catalog::SelectRow *> m_mode_rows;
    catalog::Input *m_alias_input = nullptr;
    catalog::ValidatedInput *m_origin_input = nullptr;
    catalog::ValidatedInput *m_xpub_input = nullptr;
    catalog::TextEdit *m_qr_response = nullptr;
    catalog::Button *m_decode_qr_btn = nullptr;
    catalog::Button *m_browse_btn = nullptr;
    catalog::Label *m_file_label = nullptr;
    catalog::Label *m_hot_caption = nullptr;
    catalog::Label *m_error_label = nullptr;
    catalog::Button *m_register_btn = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
    QWidget *m_alias_row = nullptr;
    QWidget *m_origin_row = nullptr;
    QWidget *m_xpub_row = nullptr;
    QWidget *m_qr_row = nullptr;
    QWidget *m_file_row = nullptr;
    QString m_file_path;
};

} // namespace keys::modal
