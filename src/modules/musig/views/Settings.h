#pragma once

#include <Qontrol>

class MusigAccountController;

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
};

} // namespace musig::view
