#pragma once

#include "theme/Palette.h"
#include <QFrame>
#include <QIcon>
#include <QString>

class QLabel;

namespace catalog {

// Status pill: an optional leading icon and a label, tinted by role. A fixed
// PillWidth centres the content.
class Badge : public QFrame {
    Q_OBJECT

public:
    enum class Role { Success, Warning, Error, Neutral, Off };

    explicit Badge(const QString &text, Role role = Role::Neutral, QWidget *parent = nullptr);
    explicit Badge(Role role = Role::Neutral, QWidget *parent = nullptr);
    void setRole(Role role);
    [[nodiscard]] auto role() const -> Role;
    void setText(const QString &text);
    [[nodiscard]] auto text() const -> QString;
    // `icon` is drawn as given: tint it with the role's IconColor.
    void setIcon(const QIcon &icon);
    void setWidth(PillWidth width);
    static auto qss(const Palette &p) -> QString;
    [[nodiscard]] static auto pillHeight() -> int;

protected:
    void applyRole();
    [[nodiscard]] static auto textFont() -> QFont;

private:
    Role m_role = Role::Neutral;
    QLabel *m_icon = nullptr;
    QLabel *m_text = nullptr;
};

} // namespace catalog
