#pragma once

#include <QPointer>
#include <QString>

#include <cstdint>
#include <optional>

#include "modules/keys/flow/SigningFlow.h"

#include <interfaces/types.h>

namespace catalog {
class ModalStatus;
} // namespace catalog

namespace keys::modal {
class Unseal;
} // namespace keys::modal

namespace keys::flow {

// Signs with a hot key, or fetches its descriptor or an xpub: the key's password is
// asked in the generic unseal modal opened over the flow, which works under its spinner
// while this route waits behind it. A wrong password asks again; closing the prompt goes
// back to the picker, or closes the flow when there is none.
class HotRouteFlow final : public SigningRouteFlow {
    Q_OBJECT

public:
    HotRouteFlow(KeysModule *module, SigningFlow::Request request, FlowRow row, QString psbt);
    // Opens the password prompt over the flow.
    void start() override;

public slots:
    void onAskPassword();
    void onPasswordEntered(const QString &password);
    void onHotSigned(ReqId req_id, bool is_ok, const QString &psbt_v2_hex, const QString &error,
                     bool wrong_password, bool musig_session);
    void onHotDescriptorFetched(ReqId req_id, bool is_ok, const QString &descriptor,
                                const QString &error, bool wrong_password);
    void onHotXpubFetched(ReqId req_id, bool is_ok, const QString &xpub, const QString &error,
                          bool wrong_password);

protected:
    // The key's answer, kept until the prompt has closed over it.
    enum class AnswerKind : std::uint8_t { Signed, Descriptor, Xpub };

    struct Answer {
        AnswerKind kind;
        bool is_ok;
        QString value;
        QString error;
        bool musig_session = false;
    };

    void init();
    void doConnect();
    void view();

    // Waiting view, behind the prompt
    void initWaiting();
    void showWaiting();
    void hideWaiting();

    void answer(AnswerKind kind, bool is_ok, const QString &value, const QString &error,
                bool wrong_password, bool musig_session);
    void deliver(const Answer &answer);

private:
    ReqId m_hot_req = 0;
    catalog::ModalStatus *m_waiting_step = nullptr;
    QPointer<modal::Unseal> m_unseal;
    std::optional<Answer> m_answer;
};

} // namespace keys::flow
