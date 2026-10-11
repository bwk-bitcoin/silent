#pragma once

#include "qr/QrCamera.h"
#include "qr/QrTypes.h"

#include <QImage>
#include <QWidget>

class QLabel;
class QPaintEvent;
class QResizeEvent;

namespace catalog {
class Label;
class Spinner;
} // namespace catalog

namespace qr {

// Shows bare camera frames: the 4:3 picture on its fixed dark surface, with the
// camera's state drawn over it while there is no live picture.
class QrCameraDisplay : public QWidget {
    Q_OBJECT

public:
    explicit QrCameraDisplay(QWidget *parent = nullptr);
    // Fixes the picture's width; its height follows the 4:3 ratio. Until set, the
    // picture takes the width its layout gives it.
    void setSize(int width);
    [[nodiscard]] auto hasHeightForWidth() const -> bool override;
    [[nodiscard]] auto heightForWidth(int width) const -> int override;

public slots:
    void frame(const qr::CameraFrame &frame);
    void setState(qr::QrCamera::State state);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QImage m_frame;
    QWidget *m_overlay = nullptr;
    catalog::Spinner *m_spinner = nullptr;
    QLabel *m_icon = nullptr;
    catalog::Label *m_title = nullptr;
    catalog::Label *m_body = nullptr;
};

} // namespace qr
