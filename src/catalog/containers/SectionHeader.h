#pragma once

#include <QString>
#include <QWidget>

struct Palette;

namespace qontrol {
class Row;
}

namespace catalog {

class Button;
class Label;

// A list section's header row: eyebrow label, optional mono count, and an action
// on the right. A foldable header leads with a chevron and toggles on click; folded,
// it hides its action.
class SectionHeader : public QWidget {
    Q_OBJECT

public:
    explicit SectionHeader(const QString &label, QWidget *parent = nullptr);
    void setLabel(const QString &label);
    // Empty hides it.
    void setCount(const QString &count);
    void setAction(QWidget *action);
    void setFoldable(bool foldable);
    void setOpen(bool open);
    [[nodiscard]] auto isOpen() const -> bool;
    // Icon-button height and a tighter gap below, for sections stacked in a modal.
    void setDense(bool dense);
    static auto qss(const Palette &p) -> QString;

signals:
    void toggled(bool open);

public slots:
    void onFoldClicked();

protected:
    void refresh();

private:
    Label *m_label = nullptr;
    Button *m_fold = nullptr;
    Label *m_count = nullptr;
    qontrol::Row *m_row = nullptr;
    QWidget *m_action = nullptr;
    bool m_foldable = false;
    bool m_open = true;
};

} // namespace catalog
