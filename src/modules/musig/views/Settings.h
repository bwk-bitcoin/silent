#pragma once

#include <QWidget>
#include <Qontrol>

class MusigAccountController;

namespace catalog {
class Button;
class CopyField;
class FieldNote;
class Input;
class Label;
} // namespace catalog

namespace musig::view {

class Settings : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Settings(MusigAccountController *ctrl);

public slots:
    void onSaveClicked();
    void onDeleteClicked();

protected:
    void init() override;
    void doConnect() override;
    void view() override;

private:
    MusigAccountController *m_controller = nullptr;
    QWidget *m_main_widget = nullptr;
    catalog::Input *m_electrum_url_input = nullptr;
    catalog::FieldNote *m_electrum_note = nullptr;
    catalog::CopyField *m_descriptor = nullptr;
    catalog::Label *m_cosigners = nullptr;
    catalog::Button *m_save_btn = nullptr;
    catalog::Button *m_delete_btn = nullptr;
};

} // namespace musig::view
