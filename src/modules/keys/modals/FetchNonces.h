#pragma once

#include "catalog/containers/Modal.h"

#include <QPointer>
#include <QString>

#include <interfaces/types.h>

namespace catalog {
class Button;
class ValidatedInput;
class ModalActions;
class ModalStatus;
class Notice;
}

namespace keys {
class KeysModule;
}

namespace keys::modal {

class Unseal;

class FetchNonces final : public catalog::Modal {
    Q_OBJECT

public:
    FetchNonces(KeysModule *module, QString fingerprint, QString descriptor, bool hot);

public slots:
    void onNextClicked();
    void onFetched(const QString &error);
    void onQuantityChanged();
    void onHotPassword(const QString &password);
    void onHotCreated(ReqId req_id, bool is_ok, const QString &error, bool wrong_password);

protected:
    void init();
    void doConnect();
    void view();
    void createHot();

private:
    KeysModule *m_module = nullptr;
    QString m_fingerprint;
    QString m_descriptor;
    catalog::ValidatedInput *m_quantity = nullptr;
    catalog::Button *m_fetch = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::ModalStatus *m_status = nullptr;
    catalog::Notice *m_error = nullptr;
    bool m_fetching = false;
    bool m_hot = false;
    bool m_hot_done = false;
    QString m_hot_error;
    QPointer<Unseal> m_unseal;
    ReqId m_hot_req = 0;
};

} // namespace keys::modal
