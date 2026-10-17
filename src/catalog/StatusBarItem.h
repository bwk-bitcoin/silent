#pragma once

#include <QWidget>

namespace catalog {

class Label;
class Toggle;

class StatusBarItem : public QWidget {
    Q_OBJECT

public:
    explicit StatusBarItem(QWidget *parent = nullptr);
    void setLabel(const QString &label);
    void setOn(bool on);
    void setToggleEnabled(bool enabled);

signals:
    void toggled(bool checked);

private:
    Toggle *m_toggle = nullptr;
    Label *m_label = nullptr;
};

} // namespace catalog
