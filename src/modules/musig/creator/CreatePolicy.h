#pragma once

#include "interfaces/types.h"

#include <QString>
#include <Qontrol>
#include <cstdint>
#include <optional>
#include <silent.h>

class QComboBox;
class QLabel;

namespace catalog {
class Button;
class Input;
} // namespace catalog

namespace musig::modal {

class PolicyEditor;
class ReviewPolicy;

// The 7-step Create flow (decision 10). Steps:
//  1 Define: name, network, PolicyEditor (paths, thresholds, timelocks, key types,
//    keys picked from IKeys::keys(), an account index per key, default 0).
//  2 Compile: preview_policy() on every edit; leaf count, cap and errors shown,
//    Next disabled while it fails.
//  3 FetchXpubs: per key cachedXpub() else xpub() at bip87_account_path(network,
//    account); then compile_policy() (R derived in Rust).
//  4 Review: review_policy() of the descriptor in plain words, leaf count, descriptor.
//  5 Register: registerDescriptor() on each own key.
//  6 VerifyAddress: verifyAddress() of receive address 0 on one own device.
//  7 Install: new_config(..., "musig")->to_file(), then Host::refreshLauncher(), and
//    the modal closes. The account is not opened.
class CreatePolicy : public qontrol::Modal {
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

public slots:
    void onNetworkChanged(int index);
    void onNameEdited(const QString &text);
    void onPolicyEdited();
    void onNextClicked();
    void onBackClicked();
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
    QComboBox *m_network_combo = nullptr;
    PolicyEditor *m_editor = nullptr;
    ReviewPolicy *m_review = nullptr;
    QLabel *m_status_label = nullptr;
    catalog::Button *m_back_btn = nullptr;
    catalog::Button *m_next_btn = nullptr;
    PolicySpec m_spec;
    QString m_descriptor;
    int m_next_index = 0;
    ReqId m_req_id = 0;
};

} // namespace musig::modal
