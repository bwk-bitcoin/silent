#pragma once

#include "catalog/containers/Modal.h"
#include "catalog/form/EndpointRow.h"
#include "interfaces/keys.h"
#include "interfaces/types.h"
#include "modules/musig/creator/SpendPath.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>
#include <silent.h>

class QWidget;

namespace catalog {
class Button;
class Card;
class Checkbox;
class ChunkedAddress;
class ComboBox;
class FieldGroup;
class Input;
class Label;
class ModalActions;
class Notice;
class SelectRow;
class Toggle;
class WizardSteps;
enum class LabelTone;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig::modal {

class PathEditor;

// Create Multikey Account, a wizard under a step trail, every step one height:
//  1 Spend Paths: the network and one row per path, each opening Edit Spend Path;
//    heights are counted from the tip of the network's last or default Electrum server.
//  2 Xpubs: one row per key of the paths, a missing one fetched on click; skipped when
//    every account xpub is cached. Then compile_policy().
//  3 Review: the account name, the policy, and Backup Descriptor before Continue.
//  4 Register: one row per signer, a device registering the descriptor on click; Skip
//    leaves devices for the Keys tab. Keys with no device register on their own.
//  5 Verify: the first receive address, one row per signer, a device with a screen
//    showing it on click. Optional.
//  6 Electrum: the server, SSL and the certificate check, then Create installs the
//    account (not opened).
class CreatePolicy : public catalog::Modal {
    Q_OBJECT

public:
    enum class View : std::uint8_t { Paths, EditPath, Xpubs, Review, Register, Verify, Electrum };

    CreatePolicy();
    // A keys-module flow closes this window and runs in its place, then shows this
    // window again with the answer.
    auto exec() -> int override;

signals:
    void electrumTestReady(ConnectionResult result);
    void tipReady(Network network, ElectrumTipResult result);

public slots:
    void onBackClicked();
    void onNextClicked();
    void onNetworkChanged(int index);
    void onPathRowClicked();
    void onAddPathClicked();
    void onRemovePathClicked();
    void onPathEdited();
    void onXpubRowClicked();
    void onNameEdited(const QString &text);
    void onBackupClicked();
    void onRegisterRowClicked();
    void onSkipToggled(bool checked);
    void onVerifyRowClicked();
    void onElectrumEdited();
    void onSslToggled(bool checked);
    void onVerifyCertificateToggled(bool checked);
    void onTestElectrum();
    void onElectrumTestReady(const ConnectionResult &result);
    void onTipReady(Network network, const ElectrumTipResult &result);
    void onXpubReady(ReqId req_id, const QString &xpub, const QString &fingerprint,
                     const QString &path);
    void onDescriptorRegistered(ReqId req_id, const QString &fingerprint);
    void onAddressVerified(ReqId req_id, bool verified);
    void onKeysError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);
    void onKeysCancelled(ReqId req_id);

protected:
    // The keys-module flow this window closed to run.
    enum class Flow : std::uint8_t { None, Xpub, Verify };

    void init();
    void doConnect();
    void view();

    // Paths view
    void initPaths();
    void connectPaths();
    [[nodiscard]] auto viewPaths() -> QWidget *;
    void showPaths();
    // One row per path, then the add row.
    void refreshPathRows();
    [[nodiscard]] auto pathsReason() -> QString;

    // EditPath view
    void initEditPath();
    void connectEditPath();
    [[nodiscard]] auto viewEditPath() -> QWidget *;
    void showEditPath(int index);
    void hideEditPath();
    void refreshEditActions();

    // Xpubs view
    void initXpubs();
    [[nodiscard]] auto viewXpubs() -> QWidget *;
    void showXpubs();
    void hideXpubs();

    // Review view
    void initReview();
    void connectReview();
    [[nodiscard]] auto viewReview() -> QWidget *;
    void showReview();
    void hideReview();
    void refreshReviewActions();

    // Register view
    void initRegister();
    void connectRegister();
    [[nodiscard]] auto viewRegister() -> QWidget *;
    void showRegister();
    void hideRegister();
    void refreshRegisterRows();
    // Registers the next key with no device, which needs no click.
    void registerNextQuiet();

    // Verify view
    void initVerify();
    [[nodiscard]] auto viewVerify() -> QWidget *;
    void showVerify();
    void hideVerify();
    void refreshVerifyRows();

    // Electrum view
    void initElectrum();
    void connectElectrum();
    [[nodiscard]] auto viewElectrum() -> QWidget *;
    void showElectrum();
    void hideElectrum();
    void refreshElectrumActions();
    void applyNetworkDefaults();
    // The address to test: the input without its scheme, under the scheme SSL picks.
    [[nodiscard]] auto electrumAddress() const -> QString;
    void install();

    // Shared
    // The step trail and the window title of a wizard step.
    void showStep(int index);
    [[nodiscard]] auto network() const -> Network;
    [[nodiscard]] auto networkKeys() const -> QList<KeyEntry>;
    // A first path over every key that signs MuSig2, two of them signing when there are
    // several.
    [[nodiscard]] auto defaultPath() const -> SpendPath;
    // Fingerprint to the name a key is shown by: its alias, else its fingerprint.
    [[nodiscard]] auto keyNames() const -> QHash<QString, QString>;
    [[nodiscard]] auto keyEntry(const QString &fingerprint) const -> std::optional<KeyEntry>;
    // The tip of the network's last or default Electrum server, off the GUI thread.
    void fetchTip();
    [[nodiscard]] auto spec() const -> PolicySpec;
    // m/87'/coin'/0' of the picked network.
    [[nodiscard]] auto accountPath() const -> QString;
    [[nodiscard]] auto missingXpubs() const -> QStringList;
    // Compiles the paths with the account xpubs, then Review; a refused policy goes
    // back to Spend Paths with the error.
    void compilePolicy();
    // Whether a key registers the descriptor on a device: one with a USB, QR or SD card
    // route.
    [[nodiscard]] auto needsRegister(const QString &fingerprint) const -> bool;
    // An error under the actions of the view on display, empty `text` hides it.
    void showStatus(const QString &text, catalog::LabelTone tone);
    // Runs the flow the window closed for, once it has closed.
    void runFlow();

private:
    View m_view = View::Paths;
    catalog::WizardSteps *m_steps = nullptr;
    catalog::Label *m_status_label = nullptr;

    // Paths view
    catalog::ComboBox *m_network_combo = nullptr;
    qontrol::Column *m_path_rows = nullptr;
    catalog::ModalActions *m_paths_actions = nullptr;
    catalog::Button *m_paths_next_btn = nullptr;
    QWidget *m_paths_page = nullptr;
    QWidget *m_paths_footer = nullptr;

    // EditPath view
    PathEditor *m_editor = nullptr;
    catalog::ModalActions *m_edit_actions = nullptr;
    catalog::Button *m_remove_path_btn = nullptr;
    catalog::Button *m_save_path_btn = nullptr;
    QWidget *m_edit_page = nullptr;
    QWidget *m_edit_footer = nullptr;
    // The path open in the editor, m_paths.size() for a new one.
    int m_editing = 0;

    // Xpubs view
    catalog::Label *m_xpubs_count = nullptr;
    qontrol::Column *m_xpub_rows = nullptr;
    catalog::ModalActions *m_xpubs_actions = nullptr;
    catalog::Button *m_xpubs_next_btn = nullptr;
    QWidget *m_xpubs_page = nullptr;
    QWidget *m_xpubs_footer = nullptr;

    // Review view
    catalog::Input *m_name_input = nullptr;
    qontrol::Column *m_review_cards = nullptr;
    catalog::Label *m_backed_up_label = nullptr;
    catalog::Button *m_backup_primary_btn = nullptr;
    catalog::Button *m_backup_btn = nullptr;
    catalog::ModalActions *m_review_actions = nullptr;
    catalog::Button *m_review_next_btn = nullptr;
    QWidget *m_review_page = nullptr;
    QWidget *m_review_footer = nullptr;

    // Register view
    qontrol::Column *m_register_rows = nullptr;
    catalog::Checkbox *m_skip_check = nullptr;
    catalog::ModalActions *m_register_actions = nullptr;
    catalog::Button *m_register_next_btn = nullptr;
    QWidget *m_register_page = nullptr;
    QWidget *m_register_footer = nullptr;

    // Verify view
    catalog::Label *m_verify_lead = nullptr;
    catalog::ChunkedAddress *m_address_label = nullptr;
    qontrol::Column *m_verify_rows = nullptr;
    catalog::Notice *m_skip_notice = nullptr;
    catalog::ModalActions *m_verify_actions = nullptr;
    catalog::Button *m_verify_next_btn = nullptr;
    QWidget *m_verify_page = nullptr;
    QWidget *m_verify_footer = nullptr;

    // Electrum view
    catalog::EndpointRow m_electrum;
    catalog::Toggle *m_ssl_toggle = nullptr;
    catalog::Toggle *m_verify_toggle = nullptr;
    catalog::ModalActions *m_electrum_actions = nullptr;
    catalog::Button *m_install_btn = nullptr;
    QWidget *m_electrum_page = nullptr;
    QWidget *m_electrum_footer = nullptr;

    QList<SpendPath> m_paths;
    std::optional<std::uint32_t> m_tip;
    bool m_skipped_xpubs = false;
    QString m_descriptor;
    bool m_backed_up = false;
    ReqId m_req_id = 0;
    Flow m_pending_flow = Flow::None;
    // The key a pending xpub fetch or address check is for.
    QString m_flow_fingerprint;
    // Distinct keys of the paths, in path order.
    QStringList m_fingerprints;
    // Fingerprint to account xpub.
    QHash<QString, QString> m_account_xpubs;
    QSet<QString> m_registered;
    // The key registering now, empty when none is.
    QString m_registering;
    QSet<QString> m_verified;
    MusigReceiveAddress m_address{};
};

} // namespace musig::modal
