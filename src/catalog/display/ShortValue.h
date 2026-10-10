#pragma once

#include <QString>
#include <QWidget>

namespace catalog {

class CopyButton;
class Label;

// A long mono value shown short (head…tail) as plain text, the full value as
// tooltip, with an optional copy button.
class ShortValue : public QWidget {
    Q_OBJECT

public:
    explicit ShortValue(const QString &value = QString(), bool copyable = true,
                        QWidget *parent = nullptr);
    void setValue(const QString &value);

private:
    Label *m_text = nullptr;
    CopyButton *m_copy = nullptr;
};

} // namespace catalog
