#pragma once

#include "catalog/containers/Modal.h"
#include "catalog/form/EndpointRow.h"
#include "interfaces/types.h"

#include <QString>
#include <cstdint>
#include <optional>
#include <silent.h>

class QWidget;

namespace catalog {
class Button;
class Card;
class ComboBox;
class FieldNote;
class Input;
class Label;
class ModalActions;
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
//  6 VerifyAddress: verifyAddress() of receive address 0 on one own device.
//  7 Install: new_config(..., "musig")->to_file(), then Host::refreshLauncher(), and
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
    auto exec() -> int override;

signals:
    void electrumTestReady(ConnectionResult result);

public slots:
    void onNetworkChanged(int index);
    void onNameEdited(const QString &text);
    void onPolicyEdited();
    void onNextClicked();
    void onBackClicked();
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
    void init();
    void doConnect();
    void view();
    void showStep(Step step);
    [[nodiscard]] auto network() const -> Network;
    [[nodiscard]] auto disabledReason() const -> QString;
    void updateActions();
    void applyNetworkDefaults();
    void previewPolicy();
    void fetchNextXpub();
    void compilePolicy();
    void showReview();
    void registerNext();
    void verifyAddress();
    void install();

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
    bool m_preview_ok = false;
    PolicySpec m_spec;
    QString m_descriptor;
    int m_next_index = 0;
    ReqId m_req_id = 0;
};

} // namespace musig::modal
