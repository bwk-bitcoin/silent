#pragma once

#include <optional>

#include <QList>
#include <QString>

#include "catalog/containers/StepModal.h"
#include "modules/keys/KeysModule.h"

#include <interfaces/types.h>
#include <silent.h>

class QDragEnterEvent;
class QDropEvent;

namespace catalog {
class Button;
class Card;
class Checkbox;
class ComboBox;
class FieldGroup;
class FieldNote;
class Input;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
class SelectRow;
class Table;
class ValidatedInput;
class WordGrid;
class WordInputGrid;
} // namespace catalog

namespace keys::flow {
class CameraStep;
class QrDisplayStep;
} // namespace keys::flow

namespace keys::modal {

// Adds a key no connected device registers: an airgap key over QR codes, a key file,
// a pasted xpub with its origin, or a hot key generated or imported here and sealed
// with a password. One modal: the mode list, then each mode's steps in its place.
class AddKey final : public catalog::StepModal {
    Q_OBJECT

public:
    explicit AddKey(KeysModule *module);
    ~AddKey() override;

public slots:
    void done(int result) override;
    void refit();
    void onModeClicked();
    void showModes();
    void onQrNext();
    void onCameraBack();
    void onQrXpub(const QString &fingerprint, const QString &path, const QString &xpub);
    void onQrScanFailed(QrErrorKind kind, const QString &message);
    void onQrFailed(const QString &message);
    void onReviewBack();
    void updateReview();
    void addReviewed();
    void openFile();
    void updatePaste();
    void addPasted();
    void onHotKindClicked();
    void showHotChoose();
    void onWordCountPicked();
    void updateHotChoose();
    void continueHot();
    void onMnemonicGenerated(ReqId req_id, const MnemonicResult &result);
    void updateBackup();
    void showHotPassword();
    void onHotPasswordBack();
    void updateHotPassword();
    void sealKey();
    void onHotKeyImported(ReqId req_id, const KeyInfoResult &result);
    void onFailureRetry();

protected:
    // Where the review step came from: what Back returns to and which route the key
    // gets.
    enum class Source { Qr, File };
    enum class HotKind { Generate12, Generate24, Import };
    enum class Failure { Qr, Seal };

    struct PastedKey {
        QString fingerprint;
        PathXpub xpub;
    };

    void init();
    void doConnect();
    void view();
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    [[nodiscard]] auto buildModes() -> QWidget *;
    [[nodiscard]] auto buildReview() -> QWidget *;
    [[nodiscard]] auto buildPaste() -> QWidget *;
    [[nodiscard]] auto buildHotChoose() -> QWidget *;
    [[nodiscard]] auto buildBackup() -> QWidget *;
    [[nodiscard]] auto buildHotPassword() -> QWidget *;
    void showQr();
    void showFile();
    void showReview(Source source);
    void loadFile(const QString &path);
    void showFileError(const QString &error);
    void showFailure(Failure failure, const QString &title, const QString &message);
    void stopQr();
    void clearSecrets();
    // Why `alias` cannot name a new key, empty when it can.
    [[nodiscard]] auto aliasReason(const QString &alias) const -> QString;
    // The alias of the key already registered with `fingerprint`, empty when none is.
    [[nodiscard]] auto registeredAlias(const QString &fingerprint) const -> QString;
    [[nodiscard]] auto pastedKey() const -> std::optional<PastedKey>;
    [[nodiscard]] auto typedMnemonic() const -> QString;
    void refreshHeader(const QString &fingerprint);
    void addKey(const QString &fingerprint, const QString &alias, const QList<KeyRouteKind> &routes,
                const QList<PathXpub> &xpubs, catalog::Notice *error);

private:
    KeysModule *m_module = nullptr;
    catalog::Label *m_header_fingerprint = nullptr;

    QList<catalog::SelectRow *> m_mode_rows;

    flow::QrDisplayStep *m_qr = nullptr;
    flow::CameraStep *m_camera = nullptr;

    Source m_source = Source::Qr;
    QString m_review_fingerprint;
    QList<PathXpub> m_review_xpubs;
    QList<KeyRouteKind> m_review_routes;
    QWidget *m_file_area = nullptr;
    catalog::Card *m_drop = nullptr;
    catalog::Notice *m_read_error = nullptr;
    catalog::Notice *m_file_error = nullptr;
    catalog::Table *m_review_table = nullptr;
    catalog::Notice *m_review_duplicate = nullptr;
    QWidget *m_review_alias_field = nullptr;
    catalog::Input *m_review_alias = nullptr;
    catalog::Notice *m_review_error = nullptr;
    catalog::Button *m_open_file = nullptr;
    catalog::Button *m_add_reviewed = nullptr;
    catalog::ModalActions *m_review_actions = nullptr;

    catalog::ValidatedInput *m_paste = nullptr;
    catalog::FieldNote *m_paste_note = nullptr;
    catalog::Input *m_paste_alias = nullptr;
    catalog::Notice *m_paste_duplicate = nullptr;
    catalog::Notice *m_paste_error = nullptr;
    catalog::Button *m_add_pasted = nullptr;
    catalog::ModalActions *m_paste_actions = nullptr;

    HotKind m_hot_kind = HotKind::Generate12;
    catalog::SelectRow *m_generate12 = nullptr;
    catalog::SelectRow *m_generate24 = nullptr;
    catalog::SelectRow *m_import = nullptr;
    catalog::ComboBox *m_word_count = nullptr;
    catalog::WordInputGrid *m_words_input = nullptr;
    catalog::Button *m_choose_continue = nullptr;
    catalog::ModalActions *m_choose_actions = nullptr;
    // The generated or typed mnemonic, until it is sealed or the window closes.
    QString m_mnemonic;
    ReqId m_generate_req = 0;
    catalog::WordGrid *m_backup_words = nullptr;
    catalog::Checkbox *m_backup_check = nullptr;
    catalog::Button *m_backup_continue = nullptr;
    catalog::ModalActions *m_backup_actions = nullptr;
    catalog::FieldGroup *m_password_fields = nullptr;
    catalog::Input *m_hot_alias = nullptr;
    catalog::Input *m_password = nullptr;
    catalog::FieldNote *m_password_short = nullptr;
    catalog::Input *m_confirm = nullptr;
    catalog::FieldNote *m_password_mismatch = nullptr;
    catalog::Button *m_seal = nullptr;
    catalog::ModalActions *m_password_actions = nullptr;
    ReqId m_seal_req = 0;

    catalog::ModalStatus *m_status = nullptr;
    catalog::Button *m_done = nullptr;
    catalog::Button *m_try_again = nullptr;
    catalog::ModalActions *m_status_actions = nullptr;
    Failure m_failure = Failure::Seal;

    int m_mode_page = 0;
    int m_qr_page = 0;
    int m_camera_page = 0;
    int m_review_page = 0;
    int m_paste_page = 0;
    int m_choose_page = 0;
    int m_backup_page = 0;
    int m_password_page = 0;
    int m_status_page = 0;
};

} // namespace keys::modal
