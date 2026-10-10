#include "catalog/containers/FoldSection.h"

#include "catalog/Button.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace catalog {

FoldSection::FoldSection(const QString &title, QWidget *parent) : QWidget(parent), m_title(title) {
    setProperty("class", "fold-section");

    m_header = new Button(title.toUpper(), ButtonRole::Menu, this);
    m_header->setIcon(icon::chevronDown(IconColor::Secondary));
    auto headerFont = m_header->font();
    headerFont.setFamily(font::MONO);
    headerFont.setPointSize(size::CAPTION);
    headerFont.setWeight(font::DEMIBOLD);
    headerFont.setLetterSpacing(QFont::PercentageSpacing, 114); // design 0.14em eyebrow tracking
    m_header->setFont(headerFont);

    m_column = (new qontrol::Column)->spacing(resolve(Spacing::XS))->push(m_header);
    m_column->into(this);

    connect(m_header, &Button::clicked, this, &FoldSection::onHeaderClicked, qontrol::UNIQUE);
}

void FoldSection::setTitle(const QString &title) {
    m_title = title;
    updateHeader();
}

void FoldSection::setContent(QWidget *content) {
    if (m_content != nullptr) {
        m_column->remove(m_content.data());
    }
    m_content = content;
    if (m_content != nullptr) {
        // Indent the body under the header label, with bottom breathing room (design).
        m_content->setContentsMargins(resolve(Spacing::M), 0, 0, resolve(Spacing::M));
        m_column->push(m_content.data());
        setMinimumWidth(m_column->sizeHint().width());
        m_content->setVisible(m_expanded);
    }
}

void FoldSection::detachContent() {
    if (m_content == nullptr) {
        return;
    }
    m_column->take(m_content.data());
    m_content = nullptr;
}

void FoldSection::setExpanded(bool expanded) {
    if (m_expanded == expanded) {
        return;
    }
    m_expanded = expanded;
    updateHeader();
    if (m_content != nullptr) {
        m_content->setVisible(m_expanded);
    }
    emit toggled(m_expanded);
}

auto FoldSection::isExpanded() const -> bool {
    return m_expanded;
}

void FoldSection::onHeaderClicked() {
    setExpanded(!m_expanded);
}

void FoldSection::updateHeader() {
    // Chevron points down when expanded, right when folded (design).
    m_header->setIcon(m_expanded ? icon::chevronDown(IconColor::Secondary)
                                  : icon::chevronRight(IconColor::Secondary));
    m_header->setText(m_title.toUpper());
}

auto FoldSection::qss(const Palette &p) -> QString {
    Q_UNUSED(p);
    // The header reads as an eyebrow label with a chevron, not a button: no
    // background or border in any state (design).
    return QString(
        "QWidget[class=\"fold-section\"] QPushButton[role=\"menu\"] { background: transparent; "
        "border: none; }"
        "QWidget[class=\"fold-section\"] QPushButton[role=\"menu\"]:hover { background: transparent; }"
        "QWidget[class=\"fold-section\"] QPushButton[role=\"menu\"]:pressed { background: transparent; }"
        "QWidget[class=\"fold-section\"] QPushButton[role=\"menu\"]:checked { background: transparent; }");
}

} // namespace catalog
