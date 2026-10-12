#pragma once

#include <QLabel>
#include <QString>

namespace catalog {

// An address rendered as monospace groups with alternating dim/full coloring,
// centered, on one or two lines (design Address.jsx ChunkedAddress). Clicking cycles
// groups of 4, groups of 6, then one unbroken string.
class ChunkedAddress : public QLabel {
    Q_OBJECT

public:
    // Auto   : two lines only for long addresses (over 56 characters), else one.
    // Single : always one line.
    // Double : two lines (when there is more than one chunk).
    enum class LineMode { Auto, Single, Double };

    explicit ChunkedAddress(QWidget *parent = nullptr);
    void setAddress(const QString &address);
    [[nodiscard]] auto address() const -> QString;
    void setLineMode(LineMode mode);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    // Characters per group, in click order; 0 is the unbroken string.
    enum class Chunking { Four = 4, Six = 6, None = 0 };

    void rebuild();

    QString m_address;
    LineMode m_mode = LineMode::Auto;
    Chunking m_chunking = Chunking::Four;
};

} // namespace catalog
