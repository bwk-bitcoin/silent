#pragma once

#include "catalog/containers/StepModal.h"
#include "interfaces/signer.h"

#include <QPointer>
#include <QString>
#include <optional>

namespace catalog {
class ModalStatus;
}

namespace keys {
class KeysModule;
}

namespace keys::modal {

class FetchXpubStep;

// Fetches the xpub at a chosen path from the key's connected device and caches it,
// opened over the key detail it returns to: picking the path, then the device wait.
class FetchXpub final : public catalog::StepModal {
    Q_OBJECT

public:
    FetchXpub(KeysModule *module, const QString &fingerprint);

public slots:
    void onFetchClicked(const QString &path);
    void onDeviceXpub(ReqId req_id, const QString &xpub);
    void onDeviceError(std::optional<ReqId> req_id, const QString &message);
    void onInstanceRemoved(const QString &id);

protected:
    void init();
    void doConnect();
    void view();
    void stopFetch();
    void showError(const QString &error);

private:
    KeysModule *m_module = nullptr;
    QString m_fingerprint;
    FetchXpubStep *m_fetch = nullptr;
    catalog::ModalStatus *m_fetching = nullptr;
    QPointer<ISigner> m_signer;
    QString m_instance;
    QString m_path;
    ReqId m_req = 0;
    int m_fetch_page = 0;
    int m_fetching_page = 0;
};

} // namespace keys::modal
