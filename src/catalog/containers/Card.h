#pragma once

#include "theme/Palette.h"
#include <QWidget>

class QPaintEvent;
class QResizeEvent;

namespace qontrol {
class Column;
}

namespace catalog {

class Card : public QWidget {
    Q_OBJECT

public:
    enum class Role { Default, Inset };
    enum class Pad { L, M, S, None };

    explicit Card(Role role = Role::Default, QWidget *parent = nullptr);
    void setRole(Role role);
    [[nodiscard]] auto role() const -> Role;
    void setContent(QWidget *content);
    void setContentMargins(int left, int top, int right, int bottom);
    void setPad(Pad pad);
    // A widget pinned over the top-left corner (e.g. a remove action).
    void setCorner(QWidget *corner);
    static auto qss(const Palette &p) -> QString;

protected:
    // A plain QWidget does not paint its QSS background/border on its own; draw
    // the styled primitive so the card chrome (surface fill + border) shows.
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void applyRole();
    void placeCorner();

private:
    Role m_role = Role::Default;
    qontrol::Column *m_column = nullptr;
    QWidget *m_content = nullptr;
    QWidget *m_corner = nullptr;
};

} // namespace catalog
