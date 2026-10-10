#pragma once

#include "catalog/containers/Card.h"
#include "theme/Icon.h"
#include <QString>

namespace qontrol {
class Column;
}

namespace catalog {

class Label;

// The block shown for an empty list or section: an inset card with an icon, a
// title, a subtitle and an optional lead action.
class EmptyState : public Card {
    Q_OBJECT

public:
    EmptyState(IconFn icon, const QString &title, const QString &subtitle = QString(),
               QWidget *parent = nullptr);
    void setTitle(const QString &title);
    void setSubtitle(const QString &subtitle);
    void setAction(QWidget *action);

private:
    qontrol::Column *m_column = nullptr;
    Label *m_title = nullptr;
    Label *m_subtitle = nullptr;
    QWidget *m_action = nullptr;
};

} // namespace catalog
