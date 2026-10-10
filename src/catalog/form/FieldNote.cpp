#include "catalog/form/FieldNote.h"

#include "catalog/display/Label.h"
#include "theme/Palette.h"

#include <QLabel>
#include <Qontrol>

namespace catalog {

FieldNote::FieldNote(const QString &text, Tone tone, IconFn icon, QWidget *parent)
    : QWidget(parent),
      m_tone(tone),
      m_icon_fn(icon) {
    m_icon = new QLabel(this);
    m_text = new Label(text, LabelRole::Caption, this);
    m_text->setWordWrap(true);

    (new qontrol::Row)->spacing(resolve(Spacing::XS))->push(m_icon)->push(m_text, 1)->into(this);

    applyTone();
}

void FieldNote::setText(const QString &text) {
    m_text->setText(text);
}

void FieldNote::setTone(Tone tone) {
    m_tone = tone;
    applyTone();
}

void FieldNote::setIcon(IconFn icon) {
    m_icon_fn = icon;
    applyTone();
}

void FieldNote::applyTone() {
    IconColor color = IconColor::Muted;
    switch (m_tone) {
    case Tone::Muted:
        m_text->setTone(LabelTone::Muted);
        break;
    case Tone::Error:
        m_text->setTone(LabelTone::Error);
        color = IconColor::Error;
        break;
    case Tone::Success:
        m_text->setTone(LabelTone::Success);
        color = IconColor::Success;
        break;
    }
    m_icon->setVisible(m_icon_fn != nullptr);
    if (m_icon_fn != nullptr) {
        m_icon->setPixmap(
            m_icon_fn(color).pixmap(metric::FIELD_NOTE_ICON_SIZE, metric::FIELD_NOTE_ICON_SIZE));
    }
}

} // namespace catalog
