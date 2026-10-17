#pragma once

#include "catalog/containers/Modal.h"
#include "catalog/form/EndpointRow.h"
#include "interfaces/types.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>
#include <silent.h>

class QWidget;

namespace catalog {
class Button;
class Card;
class ChunkedAddress;
class ComboBox;
class FieldNote;
class Input;
class Label;
class ModalActions;
class Notice;
enum class LabelTone;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig::modal {

class PolicyEditor;
class ReviewPolicy;

// The 7-step Create flow (decision 10). Steps:
//  1 Define: name, network, Electrum, PolicyEditor (paths, thresholds, timelocks, key
//    types, keys picked from IKeys::keys()); every key at BIP-87 account' = 0'.
//  2 Compile: preview_policy() on every edit; leaf count, cap and errors shown,
//    Next disabled while it fails.
//  3 FetchXpubs: per key cachedXpub() else xpub() at bip87_account_path(network, 0);
//    then compile_policy() (R derived in Rust).
//  4 Review: review_policy() of the descriptor in plain words, leaf count, descriptor.
//  5 Register: registerDescriptor() on each own key.
//  6 VerifyAddress: verifyAddress() of the first receive address on one own device;
//    skipped with a warning when no key has a USB or QR route (no device can show it).
//  7 Install: new_config(..., MUSIG_PLUGIN_ID)->to_file(), then Host::refreshLauncher(), and
//    the modal closes. The account is not opened.
class CreatePolicy : public catalog::Modal {
    Q_OBJECT

public:
    enum class Step : std::uint8_t {
        Define,
        Compile,
        FetchXpubs,
        Review,
        Register,
        VerifyAddress,
        Install,
    };

    CreatePolicy();
    // A keys-module flow closes this window and runs in its place, then shows this
    // window again with the answer.
    auto exec() -> int override;

signals:
    void electrumTestReady(ConnectionResult result);

public slots:
    void onNetworkChanged(int index);
    void onNameEdited(const QString &text);
    void onPolicyEdited();
    void onNextClicked();
    void onBackClicked();
    // Closes the window to check the address on a device.
    void onCheckAddressClicked();
    void onElectrumEdited();
    void onTestElectrum();
    void onElectrumTestReady(const ConnectionResult &result);
    void onXpubReady(ReqId req_id, const QString &xpub, const QString &fingerprint,
                     const QString &path);
    void onDescriptorRegistered(ReqId req_id, const QString &fingerprint);
    void onAddressVerified(ReqId req_id, bool verified);
    void onKeysError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);
    void onKeysCancelled(ReqId req_id);

protected:
    // The keys-module flow this window closed to run.
    enum class Flow : std::uint8_t { None, Xpubs, Verify };

    void init();
    void doConnect();
    void view();
    void showStep(Step step);
    [[nodiscard]] auto network() const -> Network;
    [[nodiscard]] auto disabledReason() const -> QString;
    void updateActions();
    // Which of next, fetch, register, verify and install the step shows, and the reason.
    void updateStepActions();
    // An error or a result under the step, empty `text` hides it.
    void showStatus(const QString &text, catalog::LabelTone tone);
    void applyNetworkDefaults();
    void previewPolicy();
    // m/87'/coin'/0' of the picked network.
    [[nodiscard]] auto accountPath() const -> QString;
    [[nodiscard]] auto missingXpubs() const -> QStringList;
    // xpub() of the first key still missing its account xpub.
    void fetchNextXpub();
    void compilePolicy();
    void showReview();
    void registerNext();
    // Some key of the policy is on a device with a screen (KeyEntry::can_show_address).
    [[nodiscard]] auto canCheckAddress() const -> bool;
    void verifyAddress();
    void install();
    // Runs the flow the window closed for, once it has closed.
    void runFlow();

private:
    Step m_step = Step::Define;
    catalog::Input *m_name_input = nullptr;
    catalog::ComboBox *m_network_combo = nullptr;
    catalog::EndpointRow m_electrum;
    PolicyEditor *m_editor = nullptr;
    ReviewPolicy *m_review = nullptr;
    QWidget *m_define_page = nullptr;
    catalog::Card *m_preview_card = nullptr;
    catalog::Label *m_preview_leaves = nullptr;
    qontrol::Column *m_preview_paths = nullptr;
    catalog::FieldNote *m_preview_error = nullptr;
    catalog::Label *m_status_label = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_next_btn = nullptr;
    catalog::Button *m_fetch_btn = nullptr;
    catalog::Button *m_register_btn = nullptr;
    catalog::Button *m_verify_btn = nullptr;
    catalog::Button *m_install_btn = nullptr;
    // m_status_label over m_actions, on every step.
    QWidget *m_footer = nullptr;
    QWidget *m_fetch_page = nullptr;
    // The fetched count and one line per key still missing its xpub.
    qontrol::Column *m_missing_column = nullptr;
    // The review, scrolling, for steps Review and Register.
    QWidget *m_review_page = nullptr;
    QWidget *m_verify_page = nullptr;
    catalog::Label *m_verify_lead = nullptr;
    catalog::ChunkedAddress *m_address_label = nullptr;
    // Whether the device showed the same address.
    catalog::Label *m_verify_status = nullptr;
    // No key can show the address: it is not checked.
    catalog::Notice *m_skip_notice = nullptr;
    bool m_preview_ok = false;
    PolicySpec m_spec;
    QString m_descriptor;
    ReqId m_req_id = 0;
    Flow m_pending_flow = Flow::None;
    // Distinct keys of m_spec, in path order.
    QStringList m_fingerprints;
    // Fingerprint to account xpub.
    QHash<QString, QString> m_account_xpubs;
    QStringList m_to_register;
    MusigReceiveAddress m_address{};
    bool m_address_verified = false;
};

} // namespace musig::modal
