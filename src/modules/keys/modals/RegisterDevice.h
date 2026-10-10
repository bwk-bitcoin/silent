#pragma once

#include <QString>
#include <QStringList>

#include "catalog/containers/StepModal.h"

namespace catalog {
class Button;
class FieldNote;
class Input;
class ModalActions;
} // namespace catalog

namespace keys::modal {

// Registers a connected device as a key: the device, the network, and an optional
// alias that falls back to the device name.
class RegisterDevice final : public catalog::StepModal {
    Q_OBJECT

public:
    RegisterDevice(const QString &name, const QString &fingerprint, const QString &network,
                   const QStringList &aliases);
    ~RegisterDevice() override = default;

signals:
    // `alias` is the device name when the field was left empty.
    void registerConfirmed(QString alias);

public slots:
    void onAliasEdited();
    void onRegisterClicked();

protected:
    void init();
    void doConnect();
    void view();
    [[nodiscard]] auto aliasTaken() const -> bool;

private:
    QString m_name;
    QString m_fingerprint;
    QString m_network;
    QStringList m_aliases;
    catalog::Input *m_alias = nullptr;
    catalog::FieldNote *m_taken = nullptr;
    catalog::Button *m_register = nullptr;
    catalog::ModalActions *m_actions = nullptr;
};

} // namespace keys::modal
