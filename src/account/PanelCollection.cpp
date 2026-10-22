#include "account/PanelCollection.h"

#include <Qontrol>

namespace account {

PanelCollection::PanelCollection(QObject *owner) : m_owner(owner) {
}

PanelCollection::~PanelCollection() {
    clear();
}

void PanelCollection::insert(qontrol::Panel *panel) {
    Q_ASSERT(!m_panels.contains(panel->name()));
    panel->setParent(m_owner);
    m_panels.insert(panel->name(), panel);
}

auto PanelCollection::contains(const QString &name) const -> bool {
    return m_panels.contains(name);
}

auto PanelCollection::value(const QString &name) const -> qontrol::Panel * {
    return m_panels.value(name);
}

auto PanelCollection::screen(const QString &name) const -> std::optional<qontrol::Screen *> {
    auto *panel = value(name);
    if (panel != nullptr) {
        return panel->screen();
    }
    return std::nullopt;
}

void PanelCollection::clear() {
    for (auto *panel : m_panels) {
        // qontrol::Panel observes its screen; layouts may have reparented it.
        delete panel->screen();
        delete panel;
    }
    m_panels.clear();
}

} // namespace account
