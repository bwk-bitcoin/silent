#include "catalog/display/Label.h"
#include "theme/Palette.h"
#include "theme/Theme.h"
#include <QFont>
#include <QStyle>

namespace catalog {

namespace {
constexpr qreal EYEBROW_TRACKING = 114;
constexpr qreal NO_TRACKING = 100;
} // namespace

Label::Label(const QString &text, LabelRole role, QWidget *parent)
    : QLabel(text, parent),
      m_role(role) {
    applyRole();
}

Label::Label(LabelRole role, QWidget *parent) : QLabel(parent), m_role(role) {
    applyRole();
}

void Label::setRole(LabelRole role) {
    if (m_role == role) {
        return;
    }
    m_role = role;
    applyRole();
}

auto Label::role() const -> LabelRole {
    return m_role;
}

void Label::setScale(qreal scale) {
    if (qFuzzyCompare(m_scale, scale)) {
        return;
    }
    m_scale = scale;
    applyRole();
}

auto Label::scale() const -> qreal {
    return m_scale;
}

void Label::setTone(LabelTone tone) {
    switch (tone) {
    case LabelTone::Default:
        setProperty("tone", "default");
        break;
    case LabelTone::Secondary:
        setProperty("tone", "secondary");
        break;
    case LabelTone::Muted:
        setProperty("tone", "muted");
        break;
    case LabelTone::Accent:
        setProperty("tone", "accent");
        break;
    case LabelTone::Success:
        setProperty("tone", "success");
        break;
    case LabelTone::Warning:
        setProperty("tone", "warning");
        break;
    case LabelTone::Error:
        setProperty("tone", "error");
        break;
    case LabelTone::OnAccent:
        setProperty("tone", "on-accent");
        break;
    case LabelTone::Inverse:
        setProperty("tone", "inverse");
        break;
    case LabelTone::InverseMuted:
        setProperty("tone", "inverse-muted");
        break;
    }
    style()->unpolish(this);
    style()->polish(this);
}

void Label::setSize(LabelSize size) {
    m_size = size;
    applyRole();
}

void Label::setTextWrapAnywhere(const QString &text) {
    // A zero-width space after every character gives QLabel a break point anywhere.
    QString breakable;
    breakable.reserve(text.size() * 2);
    for (const auto &ch : text) {
        breakable += ch;
        breakable += QChar(0x200B);
    }
    setWordWrap(true);
    setText(breakable);
}

void Label::setTextWrapped(const QString &text, int max_width) {
    setText(text);
    bool wraps = fontMetrics().horizontalAdvance(text) > max_width;
    setWordWrap(wraps);
    if (wraps) {
        setFixedWidth(max_width);
        setMinimumHeight(heightForWidth(max_width));
    } else {
        setMinimumSize(0, 0);
        setMaximumWidth(QWIDGETSIZE_MAX);
    }
}

static auto sizeForStep(LabelSize step) -> int {
    switch (step) {
    case LabelSize::Sm:
        return size::CAPTION;
    case LabelSize::Md:
        return size::BODY;
    case LabelSize::Lg:
        return size::SECTION;
    }
    return size::BODY;
}

static auto fontForRole(const FontPalette &fp, LabelRole role) -> const Font & {
    switch (role) {
    case LabelRole::Title:
        return fp.title;
    case LabelRole::Heading:
        return fp.heading;
    case LabelRole::Subheading:
        return fp.subheading;
    case LabelRole::Section:
        return fp.section;
    case LabelRole::InputLabel:
        return fp.inputLabel;
    case LabelRole::CheckboxLabel:
        return fp.checkboxLabel;
    case LabelRole::InfoLabel:
        return fp.infoLabel;
    case LabelRole::Info:
        return fp.info;
    case LabelRole::Body:
        return fp.body;
    case LabelRole::Caption:
        return fp.caption;
    case LabelRole::Mono:
        return fp.mono;
    case LabelRole::Status:
        return fp.status;
    case LabelRole::Eyebrow:
        return fp.eyebrow;
    }
    return fp.body;
}

void Label::applyRole() {
    setProperty("class", "label");

    switch (m_role) {
    case LabelRole::Body:
        setProperty("role", "body");
        break;
    case LabelRole::Title:
        setProperty("role", "title");
        break;
    case LabelRole::Heading:
        setProperty("role", "heading");
        break;
    case LabelRole::Subheading:
        setProperty("role", "subheading");
        break;
    case LabelRole::Section:
        setProperty("role", "section");
        break;
    case LabelRole::InputLabel:
        setProperty("role", "input-label");
        break;
    case LabelRole::CheckboxLabel:
        setProperty("role", "checkbox-label");
        break;
    case LabelRole::InfoLabel:
        setProperty("role", "info-label");
        break;
    case LabelRole::Info:
        setProperty("role", "info");
        break;
    case LabelRole::Caption:
        setProperty("role", "caption");
        break;
    case LabelRole::Mono:
        setProperty("role", "mono");
        break;
    case LabelRole::Status:
        setProperty("role", "status");
        break;
    case LabelRole::Eyebrow:
        setProperty("role", "eyebrow");
        break;
    }

    const auto &fp = Theme::get()->fontPalette();
    const auto &roleFont = fontForRole(fp, m_role);

    auto f = font();
    int baseSize = m_size.has_value() ? sizeForStep(*m_size) : roleFont.size;
    f.setPointSize(static_cast<int>(baseSize * m_scale));
    f.setFamily(roleFont.family);
    f.setWeight(roleFont.weight);
    bool eyebrow = m_role == LabelRole::Eyebrow;
    f.setCapitalization(eyebrow ? QFont::AllUppercase : QFont::MixedCase);
    // design 0.14em eyebrow tracking
    f.setLetterSpacing(QFont::PercentageSpacing, eyebrow ? EYEBROW_TRACKING : NO_TRACKING);

    setFont(f);

    if (roleFont.width != width::NONE) {
        setFixedWidth(roleFont.width);
    } else {
        setMinimumWidth(0);
        setMaximumWidth(QWIDGETSIZE_MAX);
    }
}

auto Label::qss(const Palette &p) -> QString {
    auto roles = QString("QLabel[class=\"label\"][role=\"caption\"] { color: %1; }"
                         "QLabel[class=\"label\"][role=\"status\"] { color: %1; }"
                         "QLabel[class=\"label\"][role=\"eyebrow\"] { color: %1; }"
                         "QLabel[class=\"label\"][role=\"info-label\"] { color: %2; }")
                     .arg(p.textMuted.name())      // %1
                     .arg(p.textSecondary.name()); // %2

    // After the role rules so a tone wins over its role's color.
    auto tones = QString("QLabel[class=\"label\"][tone=\"default\"] { color: %1; }"
                         "QLabel[class=\"label\"][tone=\"secondary\"] { color: %2; }"
                         "QLabel[class=\"label\"][tone=\"muted\"] { color: %3; }"
                         "QLabel[class=\"label\"][tone=\"accent\"] { color: %4; }"
                         "QLabel[class=\"label\"][tone=\"success\"] { color: %5; }"
                         "QLabel[class=\"label\"][tone=\"warning\"] { color: %6; }"
                         "QLabel[class=\"label\"][tone=\"error\"] { color: %7; }"
                         "QLabel[class=\"label\"][tone=\"on-accent\"] { color: %8; }"
                         "QLabel[class=\"label\"][tone=\"inverse\"] { color: %9; }"
                         "QLabel[class=\"label\"][tone=\"inverse-muted\"] { color: %10; }")
                     .arg(p.text.name())
                     .arg(p.textSecondary.name())
                     .arg(p.textMuted.name())
                     .arg(p.accent.name())
                     .arg(p.success.name())
                     .arg(p.warning.name())
                     .arg(p.error.name())
                     .arg(p.onAccent.name())
                     .arg(color::GRAY_10.name())
                     .arg(color::GRAY_40.name());

    return roles + tones;
}

} // namespace catalog
