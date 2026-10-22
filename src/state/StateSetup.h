#pragma once

#include "catalog/containers/Modal.h"

#include <QString>
#include <silent.h>

class QEvent;

namespace catalog {
class Label;
class SelectRow;
class ViewSlots;
} // namespace catalog

namespace state {

class StateSetupFlow;

// The first start: chooses what unlocks the app state. Accepted once created and
// unlocked; closing it leaves the app.
class StateSetup final : public catalog::Modal {
    Q_OBJECT

public:
    // Devices are asked on mainnet; `network` is for tests against devices running a
    // test network app.
    explicit StateSetup(Network network = Network::Bitcoin);
    void done(int result) override;

public slots:
    void onDeviceChosen();
    void onPasswordChosen();
    void onFlowViewChanged(const QString &title, ModalWidth width);
    void onFlowBack();
    void onFlowBusyChanged(bool busy);

protected:
    void init();
    void doConnect();
    void view();

    // Choice view
    void initChoice();
    void connectChoice();
    [[nodiscard]] auto viewChoice() -> QWidget *;
    void showChoice();

    // Shared
    void openFlow(StateSetupFlow *flow);
    void closeFlow();
    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    Network m_network = Network::Bitcoin;
    QWidget *m_choice = nullptr;
    catalog::Label *m_choice_text = nullptr;
    catalog::SelectRow *m_device_choice = nullptr;
    catalog::SelectRow *m_password_choice = nullptr;
    StateSetupFlow *m_flow = nullptr;
    bool m_busy = false;
};

class StateSetupFlow : public QWidget {
    Q_OBJECT

public:
    explicit StateSetupFlow(QWidget *parent = nullptr);
    ~StateSetupFlow() override = default;
    virtual void start() = 0;

signals:
    void viewChanged(const QString &title, ModalWidth width);
    void back();
    void done();
    void busyChanged(bool busy);

protected:
    [[nodiscard]] auto viewSlots() const -> catalog::ViewSlots *;

private:
    catalog::ViewSlots *m_slots = nullptr;
};

} // namespace state
