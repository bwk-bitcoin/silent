#include "catalog/containers/Section.h"

#include "catalog/containers/SectionHeader.h"

#include <Qontrol>
#include <common.h>

namespace catalog {

Section::Section(const QString &label, QWidget *parent) : QWidget(parent) {
    m_header = new SectionHeader(label, this);
    m_column = (new qontrol::Column)->push(m_header);
    m_column->into(this);
    connect(m_header, &SectionHeader::toggled, this, &Section::onHeaderToggled, qontrol::UNIQUE);
}

auto Section::header() const -> SectionHeader * {
    return m_header;
}

void Section::setContent(QWidget *content) {
    if (m_content != nullptr) {
        m_column->remove(m_content.data());
    }
    m_content = content;
    if (m_content != nullptr) {
        m_column->push(m_content.data());
        m_content->setVisible(m_header->isOpen());
    }
}

void Section::onHeaderToggled(bool open) {
    if (m_content != nullptr) {
        m_content->setVisible(open);
    }
}

} // namespace catalog
