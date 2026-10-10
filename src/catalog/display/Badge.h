#pragma once

#include "theme/Palette.h"
#include <QLabel>

namespace catalog {

class Badge : public QLabel {
    Q_OBJECT

public:
    enum class Role { Success, Warning, Error, Neutral, Off };

    explicit Badge(const QString &text, Role role = Role::Neutral, QWidget *parent = nullptr);
    explicit Badge(Role role = Role::Neutral, QWidget *parent = nullptr);
    void setRole(Role role);
    [[nodiscard]] auto role() const -> Role;
    static auto qss(const Palette &p) -> QString;

private:
    void applyRole();

    Role m_role = Role::Neutral;
};

} // namespace catalog
