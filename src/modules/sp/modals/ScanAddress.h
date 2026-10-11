#pragma once

#include "catalog/containers/Modal.h"

#include <QString>

namespace keys::flow {
class CameraStep;
} // namespace keys::flow

namespace qr {
struct QrResponse;
} // namespace qr

namespace modal {

// Reads a payment address off a QR code with the camera: the text of the first code
// read, as it is, for the address input to check.
class ScanAddress final : public catalog::Modal {
    Q_OBJECT

public:
    ScanAddress();
    ~ScanAddress() override;
    // The text read, once accepted.
    [[nodiscard]] auto text() const -> QString;

public slots:
    void onResponse(const qr::QrResponse &response);

protected:
    void init();
    void doConnect();
    void view();

private:
    keys::flow::CameraStep *m_camera = nullptr;
    QString m_text;
};

} // namespace modal
