#pragma once

#include <Qontrol>
#include <qevent.h>

namespace modal {

class CustomLaunchers : public qontrol::Modal {
    Q_OBJECT

public:
    explicit CustomLaunchers(QWidget *parent = nullptr);

public slots:
    void onCreatorClicked();

protected:
    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();
};

} // namespace modal
