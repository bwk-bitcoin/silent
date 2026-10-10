#pragma once

#include "theme/Palette.h"
#include <QIcon>
#include <QPushButton>
#include <QString>

class QTimer;

namespace catalog {

enum class ButtonRole {
    Default,
    Primary,
    Destructive,
    Menu,
    Icon,
    InlineIcon,
    TabOpen,
    TabCreate,
    Inline,
    Index
};

class Button : public QPushButton {
    Q_OBJECT

public:
    // Default time the feedback icon is shown after a click before reverting.
    static constexpr int FEEDBACK_MS = 1200;

    explicit Button(const QString &text, ButtonRole role = ButtonRole::Default,
                    QWidget *parent = nullptr);
    explicit Button(ButtonRole role = ButtonRole::Default, QWidget *parent = nullptr);
    void setRole(ButtonRole role);
    [[nodiscard]] auto role() const -> ButtonRole;
    void setWidth(BtnWidth width);
    // The pressed look of an Index button whose content is shown (e.g. its QR).
    void setActive(bool active);
    // Show `icon` (and `tooltip`, when given) transiently for `duration_ms` after each
    // click, e.g. a check mark on a copy button, then revert.
    void setFeedbackIcon(const QIcon &icon, const QString &tooltip = QString(),
                         int duration_ms = FEEDBACK_MS);
    static auto qss(const Palette &p) -> QString;

public slots:
    void onFeedbackClicked();
    void onFeedbackRestore();

protected:
    void applyRole();

private:
    ButtonRole m_role = ButtonRole::Default;
    QIcon m_base_icon;
    QIcon m_feedback_icon;
    QString m_base_tooltip;
    QString m_feedback_tooltip;
    QTimer *m_feedback_timer = nullptr;
};

} // namespace catalog
