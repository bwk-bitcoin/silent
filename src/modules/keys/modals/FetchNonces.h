#pragma once

#include "catalog/containers/Modal.h"

#include <QString>

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

class FetchNonces final : public catalog::Modal {
    Q_OBJECT

public:
    FetchNonces(KeysModule *module, QString fingerprint, QString descriptor);

public slots:
    void onNextClicked();
    void onFetched(const QString &error);
    void onQuantityChanged();

protected:
    void init();
    void doConnect();
    void view();

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
};

} // namespace keys::modal
