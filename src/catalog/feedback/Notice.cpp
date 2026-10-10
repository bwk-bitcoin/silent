#include "catalog/feedback/Notice.h"

#include "catalog/display/Label.h"
#include "theme/Icon.h"

#include <QLabel>
#include <QStyle>
#include <Qontrol>

namespace catalog {

namespace {

constexpr qreal TINT_ALPHA = 0.10;
constexpr qreal WARNING_TINT_ALPHA = 0.12;
constexpr qreal BORDER_ALPHA = 0.45;
constexpr qreal WARNING_BORDER_ALPHA = 0.48;

} // namespace

Notice::Notice(Tone tone, QWidget *parent) : QFrame(parent), m_tone(tone) {
    setProperty("class", "notice");

    m_icon = new QLabel(this);
    m_title = new Label(LabelRole::Body, this);
    auto titleFont = m_title->font();
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    m_title->setWordWrap(true);
    m_title->setVisible(false);
    m_text = new Label(LabelRole::Body, this);
    m_text->setWordWrap(true);
    m_text->setVisible(false);

    auto *body = (new qontrol::Column)->spacing(resolve(Spacing::XXS))->push(m_title)->push(m_text);

    m_row = (new qontrol::Row)
                ->margins(resolve(Padding::M), resolve(Padding::S), resolve(Padding::M),
                          resolve(Padding::S))
                ->spacing(resolve(Spacing::S))
                ->pushAligned(m_icon, Qt::AlignTop)
                ->push(body, 1);
    m_row->into(this);

    applyTone();
}

void Notice::setTone(Tone tone) {
    m_tone = tone;
    applyTone();
    style()->unpolish(this);
    style()->polish(this);
}

void Notice::setTitle(const QString &title) {
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
}

void Notice::setText(const QString &text) {
    m_text->setText(text);
    m_text->setVisible(!text.isEmpty());
}

void Notice::setAction(QWidget *action) {
    if (m_action != nullptr) {
        m_row->remove(m_action);
    }
    m_action = action;
    if (m_action != nullptr) {
        m_row->pushAligned(m_action, Qt::AlignVCenter);
    }
}

void Notice::applyTone() {
    IconFn glyph = icon::info;
    IconColor color = IconColor::Muted;
    switch (m_tone) {
    case Tone::Info:
        setProperty("tone", "info");
        break;
    case Tone::Success:
        setProperty("tone", "success");
        glyph = icon::circleCheck;
        color = IconColor::Success;
        break;
    case Tone::Warning:
        setProperty("tone", "warning");
        glyph = icon::alert;
        color = IconColor::Warning;
        break;
    case Tone::Error:
        setProperty("tone", "error");
        glyph = icon::circleX;
        color = IconColor::Error;
        break;
    }
    m_icon->setPixmap(glyph(color).pixmap(metric::NOTICE_ICON_SIZE, metric::NOTICE_ICON_SIZE));
}

auto Notice::qss(const Palette &p) -> QString {
    return QString(
               "QFrame[class=\"notice\"] { border: 1px solid transparent; "
               "border-radius: %1px; }"
               "QFrame[class=\"notice\"] QLabel { color: %2; }"
               "QFrame[class=\"notice\"][tone=\"error\"] { background: %3; border-color: %4; }"
               "QFrame[class=\"notice\"][tone=\"warning\"] { background: %5; border-color: %6; }"
               "QFrame[class=\"notice\"][tone=\"success\"] { background: %7; border-color: %8; }"
               "QFrame[class=\"notice\"][tone=\"info\"] { background: %9; border-color: %10; }"
               "QFrame[class=\"notice\"][tone=\"info\"] QLabel { color: %11; }")
        .arg(radius::BUTTON)
        .arg(p.text.name())
        .arg(qssColor(withAlpha(p.error, TINT_ALPHA)))
        .arg(qssColor(withAlpha(p.error, BORDER_ALPHA)))
        .arg(qssColor(withAlpha(p.warning, WARNING_TINT_ALPHA)))
        .arg(qssColor(withAlpha(p.warning, WARNING_BORDER_ALPHA)))
        .arg(qssColor(withAlpha(p.success, TINT_ALPHA)))
        .arg(qssColor(withAlpha(p.success, BORDER_ALPHA)))
        .arg(p.bgSecondary.name())
        .arg(p.borderLight.name())
        .arg(p.textSecondary.name());
}

} // namespace catalog
