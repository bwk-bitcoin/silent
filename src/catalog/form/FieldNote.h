#pragma once

#include "theme/Icon.h"
#include <QString>
#include <QWidget>

class QLabel;

namespace catalog {

class Label;

// A one-line note under a field: help (muted), an error or a success, with an
// optional leading icon in the same tone.
class FieldNote : public QWidget {
    Q_OBJECT

public:
    enum class Tone { Muted, Error, Success };

    explicit FieldNote(const QString &text = QString(), Tone tone = Tone::Muted,
                       IconFn icon = nullptr, QWidget *parent = nullptr);
    void setText(const QString &text);
    void setTone(Tone tone);
    void setIcon(IconFn icon);

protected:
    void applyTone();

private:
    Tone m_tone = Tone::Muted;
    IconFn m_icon_fn = nullptr;
    QLabel *m_icon = nullptr;
    Label *m_text = nullptr;
};

} // namespace catalog
