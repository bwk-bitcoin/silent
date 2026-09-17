#pragma once

#include <QString>

#include <Qontrol>

namespace catalog {
class Button;
class ModalStatus;
} // namespace catalog

namespace keys::modal {

// Spinner shown while a connected USB signer works on a request. The module drives
// it entirely: this modal never talks to a signer itself.
class UsbConfirm final : public qontrol::Modal {
    Q_OBJECT

public:
    explicit UsbConfirm(const QString &device_name, const QString &subtitle,
                        QWidget *parent = nullptr);

signals:
    void cancelled();

public slots:
    void onCancelClicked();
    void onSucceeded();
    void onFailed(const QString &message);

protected:
    void init();
    void doConnect();
    void view();

private:
    QString m_device_name;
    QString m_subtitle;
    catalog::ModalStatus *m_status = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
};

} // namespace keys::modal
