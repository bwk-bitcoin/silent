#include "catalog/panels/receive/ChunkedAddress.h"

#include "theme/Palette.h"
#include "theme/Theme.h"

#include <QEvent>
#include <QFont>
#include <QMouseEvent>
#include <QStringList>

namespace catalog {

namespace {

// Address length above which Auto mode wraps to two lines (14 groups of 4), whatever
// the grouping.
constexpr int AUTO_WRAP_LENGTH = 56;

} // namespace

ChunkedAddress::ChunkedAddress(QWidget *parent) : QLabel(parent) {
    setProperty("class", "chunked-address");
    setAlignment(Qt::AlignCenter);
    setCursor(Qt::PointingHandCursor); // click cycles the grouping
    auto labelFont = this->font();
    labelFont.setFamily(font::MONO);
    labelFont.setPointSize(size::BODY);
    setFont(labelFont);
    rebuild();
}

void ChunkedAddress::setAddress(const QString &address) {
    m_address = address;
    rebuild();
}

auto ChunkedAddress::address() const -> QString {
    return m_address;
}

void ChunkedAddress::setLineMode(LineMode mode) {
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    rebuild();
}

void ChunkedAddress::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        switch (m_chunking) {
        case Chunking::Four:
            m_chunking = Chunking::Six;
            break;
        case Chunking::Six:
            m_chunking = Chunking::None;
            break;
        case Chunking::None:
            m_chunking = Chunking::Four;
            break;
        }
        rebuild();
        return;
    }
    QLabel::mousePressEvent(event);
}

void ChunkedAddress::changeEvent(QEvent *event) {
    // Re-render with the live palette colors when the theme changes.
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        rebuild();
    }
    QLabel::changeEvent(event);
}

void ChunkedAddress::rebuild() {
    const auto &palette = Theme::get()->palette();

    const int length = static_cast<int>(m_chunking);
    if (length == 0) {
        QString text = m_address.toHtmlEscaped();
        if (wordWrap()) {
            // One unbroken string still breaks to fit: anywhere, as it has no spaces.
            text = QStringList(text.split(QString(), Qt::SkipEmptyParts)).join(QChar(0x200B));
        }
        setText(QString("<span style='color:%1'>%2</span>").arg(palette.text.name(), text));
        return;
    }

    QStringList chunks;
    for (int i = 0; i < m_address.size(); i += length) {
        chunks.append(m_address.mid(i, length));
    }

    int rows = 1;
    switch (m_mode) {
    case LineMode::Single:
        rows = 1;
        break;
    case LineMode::Double:
        rows = chunks.size() > 1 ? 2 : 1;
        break;
    case LineMode::Auto:
        rows = m_address.size() > AUTO_WRAP_LENGTH ? 2 : 1; // long silent-payment strings wrap
        break;
    }

    const int base = chunks.size() / rows;
    const int extra = chunks.size() % rows; // first `extra` rows get one more chunk
    QStringList lines;
    int idx = 0;
    int colorIdx = 0; // continuous across rows so the dim/full pattern is unbroken
    for (int r = 0; r < rows; ++r) {
        const int count = base + (r < extra ? 1 : 0);
        QStringList spans;
        for (int c = 0; c < count; ++c) {
            const QColor color = (colorIdx % 2 == 1) ? palette.textMuted : palette.text;
            spans.append(QString("<span style='color:%1'>%2</span>")
                             .arg(color.name(), chunks.at(idx + c).toHtmlEscaped()));
            colorIdx++;
        }
        idx += count;
        // With word wrap on, a line breaks between groups where it would overflow.
        lines.append(spans.join(wordWrap() ? " " : "&nbsp;"));
    }
    setText(lines.join("<br>"));
}

} // namespace catalog
