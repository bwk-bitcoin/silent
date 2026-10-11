#pragma once

#include <QString>
#include <QWidget>

#include "modules/keys/flow/SigningFlow.h"

namespace catalog {
class Button;
class ChunkedAddress;
class ModalActions;
class ModalStatus;
} // namespace catalog

namespace keys::flow {

// What the device said about the address it was asked to check, with the address
// itself. The owner puts actions(), Try Again, under it unless the address matched.
class VerifyResultView : public QWidget {
    Q_OBJECT

public:
    explicit VerifyResultView(const QString &address);
    // `device` names who answered in the lines under the title.
    void setResult(SigningFlow::VerifyResult result, const QString &device);
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void retryClicked();

protected:
    void init(const QString &address);
    void doConnect();
    void view();

private:
    catalog::ModalStatus *m_status = nullptr;
    catalog::ChunkedAddress *m_address = nullptr;
    catalog::ModalActions *m_actions = nullptr;
    catalog::Button *m_retry = nullptr;
};

} // namespace keys::flow
