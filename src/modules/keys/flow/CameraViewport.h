#pragma once

#include <QImage>
#include <QWidget>

#include <cstdint>

class QLabel;
class QPaintEvent;
class QResizeEvent;

namespace catalog {
class Label;
class Spinner;
} // namespace catalog

namespace keys::flow {

// The 4:3 camera preview on its fixed dark surface, with the camera's state drawn
// over it while there is no live picture.
class CameraViewport : public QWidget {
    Q_OBJECT

public:
    enum class State : std::uint8_t { Starting, None, Denied, Failed, Live };

    explicit CameraViewport(QWidget *parent = nullptr);
    void setState(State state);
    void setFrame(const QImage &frame);
    [[nodiscard]] auto hasHeightForWidth() const -> bool override;
    [[nodiscard]] auto heightForWidth(int width) const -> int override;

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

} // namespace keys::flow
