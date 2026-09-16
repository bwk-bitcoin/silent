#pragma once

#include <QString>

#include <Qontrol>

namespace catalog {
class Button;
class ModalStatus;
} // namespace catalog

namespace keys::modal {

// SD card airgap flow: save the unsigned PSBT to a file, then load it back once it
// has been signed on the device. Both steps live in one modal, driven by an
// explicit step rather than by stacking two modals.
class SdPicker final : public qontrol::Modal {
    Q_OBJECT

public:
    enum class Step { Save, Load };

    explicit SdPicker(const QString &key_alias, QWidget *parent = nullptr);

signals:
    void saveRequested(QString path);
    void loadRequested(QString path);
    void cancelled();

public slots:
    void onChooseClicked();
    void onCancelClicked();
    void onSaveDone(bool is_ok, const QString &error);
    void onLoadDone(bool is_ok, const QString &error);

protected:
    void init();
    void doConnect();
    void view();
    void updateStep();

private:
    QString m_key_alias;
    Step m_step = Step::Save;
    catalog::ModalStatus *m_status = nullptr;
    catalog::Button *m_choose_btn = nullptr;
    catalog::Button *m_cancel_btn = nullptr;
};

} // namespace keys::modal
