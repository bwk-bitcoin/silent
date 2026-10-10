#pragma once

#include <QString>
#include <QWidget>

namespace catalog {

class Button;
class Input;
class Label;

// A compact label editor with three states (design Receive LabelRow):
//   - no label  : an "Add a Label" inline button (plus icon)
//   - has label : the label text + a pencil edit button
//   - editing   : an input + a save (check) button
// Commits on Enter, focus-out, or the save button.
class LabelRow : public QWidget {
    Q_OBJECT

public:
    explicit LabelRow(QWidget *parent = nullptr);
    void setLabel(const QString &label);
    [[nodiscard]] auto label() const -> QString;

signals:
    void labelChanged(const QString &label);

public slots:
    void onAddClicked();
    void onEditClicked();
    void onCommit();

private:
    void updateState();

    QString m_label;
    bool m_editing = false;
    Button *m_add = nullptr;
    Label *m_text = nullptr;
    Button *m_edit = nullptr;
    Input *m_input = nullptr;
    Button *m_save = nullptr;
};

} // namespace catalog
