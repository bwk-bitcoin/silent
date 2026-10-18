#pragma once

#include <QWidget>

class MusigAccountController;

namespace catalog {
class Button;
class CopyField;
class Label;
} // namespace catalog

namespace musig {

// What a shared wallet's Settings shows below its Electrum server: its descriptor,
// its cosigners and Delete.
class SettingsExtras : public QWidget {
    Q_OBJECT

public:
    explicit SettingsExtras(MusigAccountController *ctrl);

public slots:
    void onDeleteClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    MusigAccountController *m_controller = nullptr;
    catalog::CopyField *m_descriptor = nullptr;
    catalog::Label *m_cosigners = nullptr;
    catalog::Button *m_delete_btn = nullptr;
};

} // namespace musig
