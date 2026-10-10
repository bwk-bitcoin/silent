#include "catalog/display/ShortValue.h"

#include "catalog/CopyButton.h"
#include "catalog/Variants.h"
#include "catalog/display/Label.h"
#include "theme/Palette.h"

#include <Qontrol>

namespace catalog {

namespace {

constexpr int HEAD = 8;
constexpr int TAIL = 6;

auto shorten(const QString &value) -> QString {
    if (value.size() <= HEAD + TAIL + 1) {
        return value;
    }
    return value.left(HEAD) + QChar(0x2026) + value.right(TAIL);
}

} // namespace

ShortValue::ShortValue(const QString &value, bool copyable, QWidget *parent) : QWidget(parent) {
    m_text = new Label(LabelRole::Mono, this);
    auto *row = (new qontrol::Row)->spacing(resolve(Spacing::XXS))->push(m_text);
    if (copyable) {
        m_copy = btnCopyValue();
        row->push(m_copy);
    }
    row->into(this);
    setValue(value);
}

void ShortValue::setValue(const QString &value) {
    m_text->setText(shorten(value));
    m_text->setToolTip(value);
    if (m_copy != nullptr) {
        m_copy->setTextToCopy(value);
    }
}

} // namespace catalog
