#pragma once

#include "theme/Palette.h"
#include <QWidget>

namespace qontrol {
class Row;
}

namespace catalog {

class Label;

// Multi-part QR scan progress: one segment per part, filled once received, over a
// caption counting the parts.
class ScanProgress : public QWidget {
    Q_OBJECT

public:
    explicit ScanProgress(QWidget *parent = nullptr);
    void setProgress(int received, int total);
    static auto qss(const Palette &p) -> QString;

private:
    qontrol::Row *m_segments = nullptr;
    Label *m_caption = nullptr;
};

} // namespace catalog
