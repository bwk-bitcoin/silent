#pragma once

#include <optional>

#include <QList>
#include <QString>
#include <QStringList>

#include "catalog/containers/Modal.h"
#include <interfaces/keys.h>
#include <interfaces/types.h>
#include <silent.h>

namespace catalog {
class Button;
class ComboBox;
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

class CreateAccount : public catalog::Modal {
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
    void onXpubReady(ReqId req_id, const QString &xpub, const QString &fingerprint,
                     const QString &path);
    void onDescriptorRegistered(ReqId req_id);
    void onKeysError(std::optional<ReqId> req_id, const QString &message);
    void onKeysCancelled(ReqId req_id);
    void onBackendInfoReady(const BackendInfo &info);
    void onElectrumTestReady(const ConnectionResult &result);
    void updateActions();

protected:
    // A descriptor or xpub a Get button asks the key for.
    enum class Fetch : std::uint8_t { SilentPayment, Taproot, Segwit };

    void init();
    void doConnect();
    void view();
    [[nodiscard]] auto network() const -> Network;
    [[nodiscard]] auto disabledReason() const -> QString;
    // The key list of the picked network, or the empty state saying where keys are.
    void populateKeys();
    void refreshKeyRows();
    // Forgets the descriptors fetched from the picked key.
    void clearFetched();
    void applyNetworkDefaults();
    void showFetchError(const QString &message);
    // Runs the fetch the last click asked for, once this window has closed.
    void runFetch();
    // Takes a sub-account's xpub from the key's cache, in place: true when it was there.
    auto takeCachedXpub(Fetch fetch) -> bool;
    // Keeps the sub-account descriptor built on `xpub`, or shows why it could not be.
    void applyXpub(Fetch fetch, const QString &fingerprint, const QString &xpub);
    // Registers the next of the account's descriptors with its key, then writes the
    // account once all are.
    void registerNext();
    void finishCreate();
    // The xpub path of a sub-account, at account 0 of the picked network's coin type.
    [[nodiscard]] auto subAccountPath(int purpose) const -> QString;

private:
    catalog::Input *m_name_input = nullptr;
    catalog::ComboBox *m_network_combo = nullptr;
    qontrol::Column *m_keys_column = nullptr;
    QList<catalog::SelectRow *> m_key_rows;
    QList<KeyEntry> m_keys;
    QWidget *m_fetch_row = nullptr;
    catalog::Button *m_get_sp = nullptr;
    catalog::Button *m_get_taproot = nullptr;
    catalog::Button *m_get_segwit = nullptr;
    catalog::Label *m_pick_key = nullptr;
    catalog::FieldNote *m_fetch_error = nullptr;
    EndpointRow m_blindbit;
    EndpointRow m_electrum;
    catalog::Button *m_create = nullptr;
    catalog::ModalActions *m_actions = nullptr;

    QString m_key_fp;
    QString m_descriptor;
    // The descriptor's own origin fingerprint, as spDescriptorReady proved it, so
    // onCreate() registers it under the key it belongs to.
    QString m_descriptor_fingerprint;
    Network m_descriptor_network = Network::Regtest;
    // The sub-account descriptors, built from the key's xpubs: empty for none.
    QString m_taproot_descriptor;
    QString m_segwit_descriptor;
    ReqId m_descriptor_req_id = 0;
    ReqId m_xpub_req_id = 0;
    ReqId m_register_req_id = 0;
    // The descriptors still to register before the account is written.
    QStringList m_to_register;
    // What the window closed to fetch, run by exec() in its place.
    std::optional<Fetch> m_pending_fetch;
    // The sub-account the outstanding xpub request is for.
    Fetch m_xpub_fetch = Fetch::Taproot;
};

} // namespace modal
