#pragma once

#include "theme/Icon.h"
#include "theme/Palette.h"
#include <QFrame>
#include <QString>

class QEvent;
class QKeyEvent;
class QLabel;
class QMouseEvent;

namespace qontrol {
class Row;
}

namespace catalog {

class Label;

// Full-width pick-list row: icon, title over an optional sub, then meta, status,
// trailing and chevron slots. A slot given a width keeps it even when empty, so
// the columns of stacked rows line up.
class SelectRow : public QFrame {
    Q_OBJECT

public:
    enum class Tone { Default, Error };

    explicit SelectRow(QWidget *parent = nullptr);
    // The row tints the icon: accent, muted when disabled, error in the error tone.
    void setIcon(IconFn icon, int size = metric::SELECT_ROW_ICON_SIZE);
    void setTitle(const QString &title);
    void setSub(const QString &sub);
    void setMeta(const QString &meta);
    void setMetaWidth(int width);
    void setStatus(QWidget *status);
    void setStatusWidth(int width);
    // An action at the row end; clicks on it do not activate the row.
    void setTrailing(QWidget *trailing);
    void setTrailingWidth(int width);
    // Overrides the automatic chevron (shown on a clickable row with no trailing
    // action that is not a choice).
    void setChevron(bool shown);
    // A static row is informational: no hover, no focus, no click.
    void setClickable(bool clickable);
    // Makes the row a choice; the selected one is tinted and checked.
    void setSelected(bool selected);
    // The active step of a step list: tinted like a selected row, not a choice.
    void setCurrent(bool current);
    void setTone(Tone tone);
    static auto qss(const Palette &p) -> QString;

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
    [[nodiscard]] auto isActivatable() const -> bool;
    [[nodiscard]] auto showsChevron() const -> bool;
    void refresh();
    void refreshIcon();
    void repolish();

private:
    IconFn m_icon_fn = nullptr;
    int m_icon_size = metric::SELECT_ROW_ICON_SIZE;
    QLabel *m_icon = nullptr;
    Label *m_title = nullptr;
    Label *m_sub = nullptr;
    Label *m_meta = nullptr;
    qontrol::Row *m_status_slot = nullptr;
    qontrol::Row *m_trailing_slot = nullptr;
    QLabel *m_chevron = nullptr;
    QLabel *m_check = nullptr;
    QWidget *m_status = nullptr;
    QWidget *m_trailing = nullptr;
    bool m_meta_fixed = false;
    bool m_status_fixed = false;
    bool m_trailing_fixed = false;
    bool m_clickable = true;
    bool m_selectable = false;
    bool m_selected = false;
    bool m_current = false;
    bool m_chevron_set = false;
    bool m_chevron_shown = false;
    Tone m_tone = Tone::Default;
};

} // namespace catalog
