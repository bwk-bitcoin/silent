#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

namespace qontrol {
class Column;
}

namespace catalog {

class SectionHeader;

// A titled block: a SectionHeader over its content. Folding a foldable section
// hides the content and the header's action.
class Section : public QWidget {
    Q_OBJECT

public:
    explicit Section(const QString &label, QWidget *parent = nullptr);
    // Count, action, folding and density are set on the header.
    [[nodiscard]] auto header() const -> SectionHeader *;
    void setContent(QWidget *content);

public slots:
    void onHeaderToggled(bool open);

private:
    SectionHeader *m_header = nullptr;
    qontrol::Column *m_column = nullptr;
    QPointer<QWidget> m_content;
};

} // namespace catalog
