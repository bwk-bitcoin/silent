#pragma once

#include <QHash>
#include <QString>
#include <optional>

class QObject;

namespace qontrol {
class Panel;
class Screen;
} // namespace qontrol

namespace account {

class PanelCollection {
public:
    explicit PanelCollection(QObject *owner = nullptr);
    ~PanelCollection();
    PanelCollection(const PanelCollection &) = delete;
    auto operator=(const PanelCollection &) -> PanelCollection & = delete;

    void insert(qontrol::Panel *panel);
    [[nodiscard]] auto contains(const QString &name) const -> bool;
    [[nodiscard]] auto value(const QString &name) const -> qontrol::Panel *;
    [[nodiscard]] auto screen(const QString &name) const -> std::optional<qontrol::Screen *>;
    // Controllers call this before destroying backend members used by their screens.
    void clear();

private:
    QObject *m_owner = nullptr;
    QHash<QString, qontrol::Panel *> m_panels;
};

} // namespace account
