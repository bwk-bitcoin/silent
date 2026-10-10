#include "catalog/containers/Card.h"

#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <Qontrol>

namespace catalog {

Card::Card(Role role, QWidget *parent) : QWidget(parent), m_role(role) {
    setProperty("class", "card");
    m_column = (new qontrol::Column)
                   ->margins(resolve(Padding::L), resolve(Padding::L), resolve(Padding::L),
                             resolve(Padding::L))
                   ->spacing(resolve(Spacing::S));
    m_column->into(this);
    applyRole();
}

void Card::setRole(Role role) {
    if (m_role == role) {
        return;
    }
    m_role = role;
    applyRole();
    style()->unpolish(this);
    style()->polish(this);
}

auto Card::role() const -> Role {
    return m_role;
}

void Card::setContent(QWidget *content) {
    if (m_content != nullptr) {
        m_column->remove(m_content);
    }
    m_content = content;
    if (m_content != nullptr) {
        m_column->push(m_content);
    }
}

void Card::setContentMargins(int left, int top, int right, int bottom) {
    m_column->margins(left, top, right, bottom);
}

void Card::setPad(Pad pad) {
    int padding = 0;
    switch (pad) {
    case Pad::L:
        padding = resolve(Padding::L);
        break;
    case Pad::M:
        padding = resolve(Padding::M);
        break;
    case Pad::S:
        padding = resolve(Padding::S);
        break;
    case Pad::None:
        break;
    }
    m_column->margins(padding, padding, padding, padding);
}

void Card::setCorner(QWidget *corner) {
    delete m_corner;
    m_corner = corner;
    if (m_corner != nullptr) {
        m_corner->setParent(this);
        m_corner->raise();
        m_corner->show();
        placeCorner();
    }
}

void Card::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    placeCorner();
}

void Card::placeCorner() {
    if (m_corner != nullptr) {
        m_corner->adjustSize();
        m_corner->move(resolve(Padding::XS), resolve(Padding::XS));
    }
}

void Card::applyRole() {
    switch (m_role) {
    case Role::Default:
        setProperty("role", "default");
        break;
    case Role::Inset:
        setProperty("role", "inset");
        break;
    }
}

void Card::paintEvent(QPaintEvent *event) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter painter(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &painter, this);
    QWidget::paintEvent(event);
}

auto Card::qss(const Palette &p) -> QString {
    return QString("QWidget[class=\"card\"] { background: %1; border: 1px solid %2; "
                   "border-radius: %3px; }"
                   "QWidget[class=\"card\"][role=\"inset\"] { background: %4; }")
        .arg(p.surface.name())
        .arg(p.borderLight.name())
        .arg(radius::BUTTON)
        .arg(p.bgSecondary.name());
}

} // namespace catalog
