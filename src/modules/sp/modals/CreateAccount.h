#pragma once

#include <optional>

namespace catalog {
class Button;
}

namespace catalog {
class Input;
}

namespace catalog {
class ComboBox;
}

namespace catalog {
class Label;
}

namespace catalog {
class ValidationMark;
}

#include <Qontrol>
#include <interfaces/types.h>
#include <silent.h>

namespace modal {

class CreateAccount : public qontrol::Modal {
    Q_OBJECT

public:
    explicit CreateAccount(QWidget *parent = nullptr);

signals:
    void createAccount(const QString &name, const QString &descriptor, Network network,
                       const QString &blindbit_url, const QString &electrum_url);
    void backendInfoReady(BackendInfo info);
    void electrumTestReady(ConnectionResult result);

public slots:
    void onCreate();
    void onNetworkChanged();
    void onTestBackend();
    void onTestElectrum();
    void onUpdateCreateButton();
    void onKeyChanged();
    void onFetchDescriptor();
    void onSpDescriptorReady(ReqId req_id, QString descriptor, QString fingerprint);
    void onDescriptorRegistered(ReqId req_id);
    void onKeysError(std::optional<ReqId> req_id, QString message);
    void onKeysCancelled(ReqId req_id);
    void onBackendInfoReady(BackendInfo info);
    void onElectrumTestReady(ConnectionResult result);

protected:
    void init();
    void doConnect();
    void view();
    void populateKeys();
    void applyNetworkDefaults();
    auto applyRegtestDefaults() -> bool;
    void invalidateBackendTest();
    void invalidateElectrumTest();

private:
    catalog::Input *m_name_input = nullptr;
    catalog::ComboBox *m_key_combo = nullptr;
    catalog::Button *m_fetch_btn = nullptr;
    catalog::Label *m_descriptor_label = nullptr;
    catalog::ValidationMark *m_descriptor_status = nullptr;
    catalog::ComboBox *m_network_combo = nullptr;
    catalog::Input *m_blindbit_input = nullptr;
    catalog::Button *m_test_btn = nullptr;
    catalog::ValidationMark *m_backend_status = nullptr;
    catalog::Input *m_electrum_input = nullptr;
    catalog::Button *m_test_electrum_btn = nullptr;
    catalog::ValidationMark *m_electrum_status = nullptr;
    catalog::Button *m_create_btn = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
    bool m_backend_verified = false;
    bool m_electrum_verified = false;
    QString m_descriptor;
    // The descriptor's own validated origin fingerprint, as returned by
    // spDescriptorReady, kept so onCreate() registers the descriptor under the key
    // it was actually proven to belong to rather than whatever the key combo
    // currently shows.
    QString m_descriptor_fingerprint;
    ReqId m_descriptor_req_id = 0;
    ReqId m_register_req_id = 0;
};

} // namespace modal
