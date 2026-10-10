#include "catalog/feedback/ScanProgress.h"

#include "catalog/display/Label.h"
#include "i18n/Tr.h"

#include <QFrame>
#include <Qontrol>

namespace catalog {

ScanProgress::ScanProgress(QWidget *parent) : QWidget(parent) {
    m_segments = new qontrol::Row;
    m_caption = new Label(LabelRole::Caption, this);
    m_caption->setAlignment(Qt::AlignCenter);

    (new qontrol::Column)
        ->spacing(resolve(Spacing::XS))
        ->push(m_segments)
        ->push(m_caption)
        ->into(this);

    setProgress(0, 0);
}

void ScanProgress::setProgress(int received, int total) {
    // clear() also resets the layout spacing
    m_segments->clear();
    m_segments->spacing(resolve(Spacing::XXS));
    for (int index = 0; index < total; ++index) {
        auto *segment = new QFrame;
        segment->setProperty("class", "scan-segment");
        segment->setProperty("on", index < received);
        segment->setFixedHeight(metric::SCAN_PROGRESS_HEIGHT);
        m_segments->push(segment, 1);
    }

    bool done = total > 0 && received >= total;
    m_caption->setTone(done ? LabelTone::Success : LabelTone::Muted);
    if (total == 0) {
        m_caption->setText(TR("scan-progress-waiting"));
    } else if (done) {
        m_caption->setText(TR("scan-progress-done").arg(total));
    } else {
        m_caption->setText(TR("scan-progress-parts").arg(received).arg(total));
    }
}

auto ScanProgress::qss(const Palette &p) -> QString {
    return QString("QFrame[class=\"scan-segment\"] { background: %1; border: none; "
                   "border-radius: %2px; }"
                   "QFrame[class=\"scan-segment\"][on=\"true\"] { background: %3; }")
        .arg(p.borderLight.name())
        .arg(radius::ICON)
        .arg(p.accent.name());
}

} // namespace catalog
