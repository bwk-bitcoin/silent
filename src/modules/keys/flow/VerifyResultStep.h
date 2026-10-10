#pragma once

#include <QString>
#include <QWidget>

#include <cstdint>

namespace catalog {
class Button;
class ChunkedAddress;
class ModalActions;
class ModalStatus;
} // namespace catalog

namespace keys::flow {

// What the device said about the address, with the address itself, and Try Again
// unless it matched.
class VerifyResultStep : public QWidget {
    Q_OBJECT

public:
    enum class Result : std::uint8_t { Matched, Mismatch, Error };

    explicit VerifyResultStep(QWidget *parent = nullptr);
    void setAddress(const QString &address);
    void setResult(Result result, const QString &device);
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void retryClicked();

protected:
    void init();
    void doConnect();
    void view();

private:
    catalog::ModalStatus *m_status = nullptr;
    catalog::ChunkedAddress *m_address = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_retry = nullptr;
};

} // namespace keys::flow
