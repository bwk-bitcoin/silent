#pragma once

#include "qr/QrTypes.h"

#include <QWidget>

namespace catalog {
class Label;
class QrCode;
} // namespace catalog

namespace qr {

// Shows bare display frames: the code on its quiet zone, dimmed under "Rebuilding"
// while the frames are rendered, over the part count.
class QrDisplay : public QWidget {
    Q_OBJECT

public:
    explicit QrDisplay(QWidget *parent = nullptr);
    // False shows a plain code, smaller and without the part count.
    void setTunable(bool tunable);

public slots:
    void frame(const qr::QrFrame &frame);

protected:
    void setRenderingShown(bool rendering);

private:
    catalog::QrCode *m_code = nullptr;
    QWidget *m_rendering = nullptr;
    catalog::Label *m_part = nullptr;
    bool m_tunable = true;
};

} // namespace qr
