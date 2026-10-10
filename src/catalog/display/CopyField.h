#pragma once

#include "theme/Palette.h"
#include <QFrame>
#include <QString>

class QResizeEvent;

namespace catalog {

class CopyButton;
class Label;

// A read-only mono value (xpub, descriptor, path) in an input-styled box with a
// copy button. Long values wrap at any character, or truncate with the full value
// as tooltip.
class CopyField : public QFrame {
    Q_OBJECT

public:
    explicit CopyField(const QString &value = QString(), QWidget *parent = nullptr);
    void setValue(const QString &value);
    void setWrap(bool wrap);
    static auto qss(const Palette &p) -> QString;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void refreshText();

private:
    QString m_value;
    bool m_wrap = false;
    Label *m_text = nullptr;
    CopyButton *m_copy = nullptr;
};

} // namespace catalog
