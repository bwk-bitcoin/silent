#pragma once

#include <QString>

#include <cstdint>

#include "modules/keys/flow/SigningFlow.h"

#include <interfaces/types.h>

namespace catalog {
class Button;
class Input;
class Label;
class ModalActions;
class ModalStatus;
class Notice;
} // namespace catalog

namespace keys::flow {

// Signs with a hot key, or fetches its descriptor: asks the key's password, then works
// under a spinner. A wrong password asks again.
class HotRouteFlow final : public SigningRouteFlow {
    Q_OBJECT

public:
    HotRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row, QString psbt);
    ~HotRouteFlow() override;
    // Asks the password.
    void start() override;

public slots:
    void onNextClicked();
    void onPasswordEdited();
    void onHotSigned(ReqId req_id, bool is_ok, const QString &psbt_v2_hex, const QString &error,
                     bool wrong_password);
    void onHotDescriptorFetched(ReqId req_id, bool is_ok, const QString &descriptor,
                                const QString &error, bool wrong_password);
    void onHotXpubFetched(ReqId req_id, bool is_ok, const QString &xpub, const QString &error,
                          bool wrong_password);

protected:
    // The view on display: the primary action acts on it.
    enum class View : std::uint8_t { Password, Busy };

    void init();
    void doConnect();
    void view();

    // Password view
    void initPassword();
    void connectPassword();
    [[nodiscard]] auto viewPassword() -> QWidget *;
    void showPassword();
    void proceedPassword();

    // Busy view
    void initBusy();
    void showBusy();
    void hideBusy();

private:
    View m_view = View::Password;
    ReqId m_hot_req = 0;

    // The value is handed over once and cleared; a QString cannot be zeroized, so this
    // is best effort.
    QWidget *m_password_step = nullptr;
    catalog::Label *m_password_prompt = nullptr;
    catalog::Input *m_password_input = nullptr;
    catalog::Notice *m_password_error = nullptr;
    catalog::ModalActions *m_password_actions = nullptr;
    catalog::Button *m_password_proceed = nullptr;

    catalog::ModalStatus *m_busy_step = nullptr;
};

} // namespace keys::flow
