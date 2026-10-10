#pragma once

#include <optional>

#include <QList>
#include <QString>

#include "catalog/containers/StepModal.h"
#include <interfaces/keys.h>
#include <interfaces/types.h>
#include <silent.h>

namespace catalog {
class Button;
class ComboBox;
class CopyField;
class FieldNote;
class Input;
class Label;
class ModalActions;
class SelectRow;
class ValidationMark;
} // namespace catalog

namespace qontrol {
class Column;
class Row;
} // namespace qontrol

namespace modal {

// A backend endpoint: a mono input, its Test button and mark, and under it the
// reason the last test failed.
class EndpointRow {
public:
    enum class State { Untested, Testing, Ok, Failed };

    EndpointRow();
    [[nodiscard]] auto control() const -> QWidget *;
    [[nodiscard]] auto note() const -> catalog::FieldNote *;
    [[nodiscard]] auto input() const -> catalog::Input *;
    [[nodiscard]] auto testButton() const -> catalog::Button *;
    [[nodiscard]] auto state() const -> State;
    // `error` is shown under the field in the Failed state.
    void setState(State state, const QString &error = QString());

private:
    State m_state = State::Untested;
    catalog::Input *m_input = nullptr;
    catalog::Button *m_test = nullptr;
    catalog::Button *m_testing = nullptr;
    catalog::ValidationMark *m_mark = nullptr;
    catalog::FieldNote *m_note = nullptr;
    QWidget *m_control = nullptr;
};

class CreateAccount : public catalog::StepModal {
    Q_OBJECT

public:
    CreateAccount();
    // The descriptor fetch closes this window and runs the keys module's flow in its
    // place, then shows this window again with the answer.
    auto exec() -> int override;

signals:
    void backendInfoReady(BackendInfo info);
    void electrumTestReady(ConnectionResult result);

public slots:
    void onCreate();
    void onNetworkChanged();
    void onKeyClicked();
    void onFetchClicked();
    void onOpenKeysTab();
    void onTestBlindbit();
    void onTestElectrum();
    void onBlindbitEdited();
    void onElectrumEdited();
    void onSpDescriptorReady(ReqId req_id, const QString &descriptor, const QString &fingerprint);
    void onDescriptorRegistered(ReqId req_id);
    void onKeysError(std::optional<ReqId> req_id, const QString &message);
    void onKeysCancelled(ReqId req_id);
    void onBackendInfoReady(const BackendInfo &info);
    void onElectrumTestReady(const ConnectionResult &result);
    void updateActions();

protected:
    void init();
    void doConnect();
    void view();
    [[nodiscard]] auto network() const -> Network;
    [[nodiscard]] auto disabledReason() const -> QString;
    // The key list of the picked network, or the empty state saying where keys are.
    void populateKeys();
    void refreshKeyRows();
    void applyNetworkDefaults();
    void showFetchError(const QString &message);
    void fetchDescriptor();
    // Resizes the window to its content after a section grew or shrank.
    void refit();

private:
    catalog::Input *m_name_input = nullptr;
    catalog::ComboBox *m_network_combo = nullptr;
    qontrol::Column *m_keys_column = nullptr;
    QList<catalog::SelectRow *> m_key_rows;
    QList<KeyEntry> m_keys;
    QWidget *m_fetch_row = nullptr;
    catalog::Button *m_get_descriptor = nullptr;
    catalog::Button *m_get_again = nullptr;
    catalog::Label *m_pick_key = nullptr;
    catalog::FieldNote *m_fetch_error = nullptr;
    catalog::CopyField *m_descriptor_field = nullptr;
    EndpointRow m_blindbit;
    EndpointRow m_electrum;
    catalog::Button *m_create = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    int m_step = 0;

    QString m_key_fp;
    QString m_descriptor;
    // The descriptor's own origin fingerprint, as spDescriptorReady proved it, so
    // onCreate() registers it under the key it belongs to.
    QString m_descriptor_fingerprint;
    Network m_descriptor_network = Network::Regtest;
    ReqId m_descriptor_req_id = 0;
    ReqId m_register_req_id = 0;
    bool m_fetch_requested = false;
};

} // namespace modal
