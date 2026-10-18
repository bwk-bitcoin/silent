#pragma once

#include <QLabel>
#include <QString>

namespace catalog {

// A muted info glyph next to a label: hovering it shows `text` in a tooltip.
class InfoTip : public QLabel {
    Q_OBJECT

public:
    explicit InfoTip(const QString &text, QWidget *parent = nullptr);
};

} // namespace catalog
