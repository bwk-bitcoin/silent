#pragma once

#include <QString>
#include <QWidget>

class QPushButton;

namespace qontrol {
class Row;
}

namespace catalog {

class Button;
class Label;

// A modal step's button bar: an optional Back on the left, then the reason the
// primary action is disabled, then the actions with the primary last.
class ModalActions : public QWidget {
    Q_OBJECT

public:
    enum class Back { None, Back, Previous, OtherSigner, AllSettings };

    explicit ModalActions(QWidget *parent = nullptr);
    void setBack(Back back);
    // Empty hides it.
    void setReason(const QString &reason);
    // Appended after the previous ones: add the primary action last.
    void addAction(QPushButton *action);

signals:
    void backClicked();

private:
    qontrol::Row *m_row = nullptr;
    Button *m_back = nullptr;
    Label *m_reason = nullptr;
};

} // namespace catalog
