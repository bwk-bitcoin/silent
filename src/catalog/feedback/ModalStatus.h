#pragma once

#include "theme/Icon.h"
#include <QString>
#include <QWidget>

class QLabel;

namespace qontrol {
class Column;
}

namespace catalog {

class Label;
class Spinner;

// Centered status column for working and result steps: a spinner or an icon, a
// title and a subtitle, each hidden while empty.
class ModalStatus : public QWidget {
    Q_OBJECT

public:
    explicit ModalStatus(QWidget *parent = nullptr);
    void setSpinner();
    void setIcon(IconFn icon, IconColor color = IconColor::Muted);
    void setTitle(const QString &title);
    void setSubtitle(const QString &subtitle);
    // Space above and below the column (design `pad`).
    void setPadding(int top, int bottom);

private:
    qontrol::Column *m_column = nullptr;
    Spinner *m_spinner = nullptr;
    QLabel *m_icon = nullptr;
    Label *m_title = nullptr;
    Label *m_subtitle = nullptr;
};

} // namespace catalog
