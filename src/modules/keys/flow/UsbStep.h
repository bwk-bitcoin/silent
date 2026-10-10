#pragma once

#include <QString>
#include <QWidget>

#include <cstdint>

namespace catalog {
class Button;
class Card;
class ChunkedAddress;
class ModalActions;
class ModalStatus;
} // namespace catalog

namespace keys::flow {

// A connected device working on the request: a spinner while it waits for the user,
// then why it failed, with Back to the other signers and Try Again.
class UsbStep : public QWidget {
    Q_OBJECT

public:
    enum class Error : std::uint8_t { Rejected, Disconnected, Failed };

    explicit UsbStep(QWidget *parent = nullptr);
    // `address`, when set, is shown under the status for the user to compare.
    void showWaiting(const QString &title, const QString &text, const QString &address);
    void showError(Error error, const QString &device);
    void setBackVisible(bool visible);
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void backClicked();
    void retryClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    catalog::ModalStatus *m_status = nullptr;
    catalog::Card *m_address_card = nullptr;
    catalog::ChunkedAddress *m_address = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_retry = nullptr;
};

} // namespace keys::flow
