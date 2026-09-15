#pragma once

namespace catalog {
class Button;
}

namespace catalog {
class Input;
}

namespace catalog {
class TextEdit;
}

namespace catalog {
class QrCode;
}

namespace catalog {
class ComboBox;
}

namespace catalog {
class ValidationMark;
}

#include <Qontrol>
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
    void onImportQrResponse();
    void onBackendInfoReady(BackendInfo info);
    void onElectrumTestReady(ConnectionResult result);

protected:
    void init();
    void doConnect();
    void view();
    void applyNetworkDefaults();
    auto applyRegtestDefaults() -> bool;
    void invalidateBackendTest();
    void invalidateElectrumTest();

private:
    catalog::Input *m_name_input = nullptr;
    catalog::QrCode *m_qr_code = nullptr;
    catalog::TextEdit *m_qr_request = nullptr;
    catalog::TextEdit *m_qr_response = nullptr;
    catalog::Button *m_qr_import_btn = nullptr;
    catalog::TextEdit *m_descriptor_input = nullptr;
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
};

} // namespace modal
