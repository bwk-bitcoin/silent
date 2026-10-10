#include "catalog/display/CopyField.h"

#include "catalog/CopyButton.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"

#include <QFontMetrics>
#include <QResizeEvent>
#include <Qontrol>

namespace catalog {

CopyField::CopyField(const QString &value, QWidget *parent) : QFrame(parent) {
    setProperty("class", "copy-field");

    m_text = new Label(LabelRole::Mono, this);
    m_text->setSize(LabelSize::Sm);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_copy = btnCopyValue();

    (new qontrol::Row)
        ->margins(resolve(Padding::S), resolve(Padding::XS), resolve(Padding::XS),
                  resolve(Padding::XS))
        ->spacing(resolve(Spacing::XS))
        ->push(m_text, 1)
        ->pushAligned(m_copy, Qt::AlignTop)
        ->into(this);

    setValue(value);
}

void CopyField::setValue(const QString &value) {
    m_value = value;
    m_copy->setTextToCopy(value);
    refreshText();
}

void CopyField::setWrap(bool wrap) {
    m_wrap = wrap;
    refreshText();
}

void CopyField::resizeEvent(QResizeEvent *event) {
    QFrame::resizeEvent(event);
    if (!m_wrap) {
        refreshText();
    }
}

void CopyField::refreshText() {
    if (m_wrap) {
        m_text->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        m_text->setToolTip(QString());
        m_text->setTextWrapAnywhere(m_value);
        return;
    }
    m_text->setWordWrap(false);
    // Let the label shrink below its text, then fill it with the elided value.
    m_text->setMinimumWidth(0);
    m_text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_text->setText(
        QFontMetrics(m_text->font()).elidedText(m_value, Qt::ElideRight, m_text->width()));
    m_text->setToolTip(m_value);
}

auto CopyField::qss(const Palette &p) -> QString {
    return QString("QFrame[class=\"copy-field\"] { background: %1; border: 1px solid %2; "
                   "border-radius: %3px; }")
        .arg(p.inputBg.name())
        .arg(p.inputBorder.name())
        .arg(radius::INPUT);
}

} // namespace catalog
