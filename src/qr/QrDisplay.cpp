#include "qr/QrDisplay.h"

#include "catalog/display/Label.h"
#include "catalog/feedback/Spinner.h"
#include "catalog/panels/receive/QrCode.h"
#include "i18n/Tr.h"
#include "theme/Palette.h"

#include <QGridLayout>

#include <Qontrol>

namespace qr {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

QrDisplay::QrDisplay(QWidget *parent) : QWidget(parent) {
    m_code = new catalog::QrCode;
    setSize(metric::QR_DISPLAY_SIZE);
    m_code->setPadding(metric::QR_DISPLAY_PADDING);
    m_code->setRadius(radius::QR_CARD);

    auto *renderingText = new Label(TR("flow-qr-rebuilding"), LabelRole::Body);
    auto renderingFont = renderingText->font();
    renderingFont.setWeight(font::DEMIBOLD);
    renderingText->setFont(renderingFont);
    m_rendering =
        (new qontrol::Column)
            ->spacing(resolve(Spacing::S))
            ->pushAligned(new catalog::Spinner(metric::QR_REBUILD_SPINNER_SIZE), Qt::AlignHCenter)
            ->pushAligned(renderingText, Qt::AlignHCenter);

    m_part = new Label(LabelRole::Mono);
    m_part->setTone(LabelTone::Secondary);
    auto partFont = m_part->font();
    partFont.setWeight(font::DEMIBOLD);
    m_part->setFont(partFont);
    m_part->setAlignment(Qt::AlignCenter);

    // The rendering overlay sits on top of the code, in the same grid cell.
    auto *codeCell = new QWidget;
    auto *grid = new QGridLayout(codeCell);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->addWidget(m_code, 0, 0, Qt::AlignCenter);
    grid->addWidget(m_rendering, 0, 0, Qt::AlignCenter);

    (new qontrol::Column)
        ->spacing(resolve(Spacing::M))
        ->pushAligned(codeCell, Qt::AlignHCenter)
        ->push(m_part)
        ->into(this);

    setRenderingShown(true);
}

void QrDisplay::setSize(int size) {
    m_code->setFixedSize(size, size);
}

void QrDisplay::setTunable(bool tunable) {
    m_tunable = tunable;
    m_code->setPadding(tunable ? metric::QR_DISPLAY_PADDING : metric::QR_REQUEST_PADDING);
    m_code->setRadius(tunable ? radius::QR_CARD : radius::BUTTON);
    m_part->setVisible(tunable);
}

void QrDisplay::frame(const QrFrame &frame) {
    m_code->setBackground(frame.background);
    if (!frame.error.isEmpty()) {
        setRenderingShown(false);
        m_part->setVisible(true);
        m_part->setText(TR("keys-qr-frame-failed").arg(frame.error));
        return;
    }
    m_part->setVisible(m_tunable);
    bool rendering = frame.image.isNull();
    setRenderingShown(rendering);
    if (rendering) {
        m_part->setText(TR("flow-qr-part-pending"));
        return;
    }
    m_code->setImage(frame.image);
    m_part->setText(TR("keys-qr-part").arg(frame.index + 1).arg(frame.count));
}

void QrDisplay::setRenderingShown(bool rendering) {
    m_code->setDimmed(rendering);
    m_rendering->setVisible(rendering);
}

} // namespace qr
