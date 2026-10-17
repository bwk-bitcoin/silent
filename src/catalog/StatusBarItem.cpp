#include "catalog/StatusBarItem.h"

#include "catalog/display/Label.h"
#include "catalog/inputs/Toggle.h"
#include "theme/Palette.h"

#include <QSignalBlocker>
#include <Qontrol>
#include <common.h>

namespace catalog {

StatusBarItem::StatusBarItem(QWidget *parent) : QWidget(parent) {
    m_toggle = new Toggle(ToggleRole::Status, this);
    m_label = new Label(LabelRole::Status, this);

    (new qontrol::Row)->spacing(resolve(Spacing::XS))->push(m_toggle)->push(m_label)->into(this);

    connect(m_toggle, &QCheckBox::toggled, this, &StatusBarItem::toggled, qontrol::UNIQUE);
}

void StatusBarItem::setLabel(const QString &label) {
    m_label->setText(label);
}

void StatusBarItem::setOn(bool on) {
    // Shows a state, it does not ask for one: toggled() is the user's switch only, or
    // showing a started scanner would start it again.
    QSignalBlocker blocker(m_toggle);
    m_toggle->setChecked(on);
}

void StatusBarItem::setToggleEnabled(bool enabled) {
    m_toggle->setEnabled(enabled);
}

} // namespace catalog
