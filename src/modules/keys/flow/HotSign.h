#pragma once

#include <QObject>
#include <QString>

#include <interfaces/types.h>
#include <silent.h>

namespace keys {
class KeysModule;
}

namespace keys::flow {

// A named hot key signs after its password window closes. The caller's row
// stays busy while the worker signs, leaving its other signers available.
class HotSign final : public QObject {
    Q_OBJECT

public:
    HotSign(KeysModule *module, ReqId bus_req, Network network, QString fingerprint, QString alias,
            QString psbt);
    void start();

signals:
    void signedPsbt(ReqId req_id, QString psbt);
    void failed(ReqId req_id, QString message, plugin::SignError kind);
    void cancelled(ReqId req_id);
    void finished();

public slots:
    void onPasswordEntered(const QString &password);
    void onHotSigned(ReqId req_id, bool is_ok, const QString &psbt_v2_hex, const QString &error,
                     bool wrong_password, bool musig_session);

private:
    KeysModule *m_module = nullptr;
    ReqId m_bus_req = 0;
    ReqId m_hot_req = 0;
    Network m_network;
    QString m_fingerprint;
    QString m_alias;
    QString m_psbt;
    bool m_submitted = false;
    bool m_wrong_password = false;
};

} // namespace keys::flow
