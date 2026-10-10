#pragma once

#include "theme/Palette.h"
#include <QFrame>
#include <QString>

class QLabel;

namespace qontrol {
class Row;
}

namespace catalog {

class Label;

// Inline message block: a tone icon, an optional title over the text, and an
// optional action on the right (dismiss, scan again).
class Notice : public QFrame {
    Q_OBJECT

public:
    enum class Tone { Info, Success, Warning, Error };

    explicit Notice(Tone tone = Tone::Info, QWidget *parent = nullptr);
    void setTone(Tone tone);
    void setTitle(const QString &title);
    void setText(const QString &text);
    void setAction(QWidget *action);
    static auto qss(const Palette &p) -> QString;

protected:
    void applyTone();

private:
    Tone m_tone = Tone::Info;
    QLabel *m_icon = nullptr;
    Label *m_title = nullptr;
    Label *m_text = nullptr;
    qontrol::Row *m_row = nullptr;
    QWidget *m_action = nullptr;
};

} // namespace catalog
