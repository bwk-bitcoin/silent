#pragma once

#include <QWidget>

namespace catalog {

// Empties a qontrol Column or Row of its widgets. Its clear() only deletes them
// later, from the event loop running when it is called: a list rebuilt outside a
// modal's own loop (before its exec(), between two exec() calls) would keep its old
// rows drawn under the new ones until that loop runs again. Hidden first, they never
// show. Like clear(), this also resets the layout spacing.
template <typename Container> void clearRows(Container *container) {
    for (auto *row : container->template findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
        row->hide();
    }
    container->clear();
}

} // namespace catalog
