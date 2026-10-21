#pragma once

#include "theme/Palette.h"
#include <QFrame>
#include <QIcon>
#include <QString>

class QLabel;

namespace qontrol {
class Row;
}

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
    // One step smaller (font, padding and width), as a pill inside a table row.
    void setDense(bool dense);
    static auto qss(const Palette &p) -> QString;
    [[nodiscard]] static auto pillHeight(bool dense = false) -> int;

protected:
    void applyRole();
    void applyMetrics();
    [[nodiscard]] static auto textFont(bool dense) -> QFont;

private:
    Role m_role = Role::Neutral;
    PillWidth m_width = PillWidth::Auto;
    bool m_dense = false;
    qontrol::Row *m_row = nullptr;
    QLabel *m_icon = nullptr;
    QLabel *m_text = nullptr;
};

} // namespace catalog
