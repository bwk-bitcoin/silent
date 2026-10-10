#include "catalog/display/WordGrid.h"

#include "catalog/display/Label.h"

#include <QFrame>
#include <QGridLayout>
#include <Qontrol>

namespace catalog {

namespace {

constexpr int COLUMNS = 3;
constexpr int WIDE_COLUMNS = 4;
constexpr int WIDE_ABOVE = 12;
const QString MASK = QString(6, QChar(0x2022));

} // namespace

WordGrid::WordGrid(QWidget *parent) : QWidget(parent) {
    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setHorizontalSpacing(resolve(Spacing::S));
    m_grid->setVerticalSpacing(resolve(Spacing::XS));
}

void WordGrid::setWords(const QStringList &words) {
    m_words = words;
    rebuild();
}

void WordGrid::setMasked(bool masked) {
    m_masked = masked;
    rebuild();
}

void WordGrid::rebuild() {
    while (auto *item = m_grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    int columns = m_words.size() > WIDE_ABOVE ? WIDE_COLUMNS : COLUMNS;
    int rows = static_cast<int>((m_words.size() + columns - 1) / columns);
    for (int index = 0; index < m_words.size(); ++index) {
        auto *number = new Label(QString::number(index + 1), LabelRole::Mono);
        number->setSize(LabelSize::Sm);
        number->setTone(LabelTone::Muted);
        number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        number->setFixedWidth(metric::WORD_INDEX_WIDTH);

        auto *word = new Label(m_masked ? MASK : m_words.at(index), LabelRole::Mono);
        auto wordFont = word->font();
        wordFont.setWeight(QFont::DemiBold);
        word->setFont(wordFont);

        auto *cell = new QFrame;
        cell->setProperty("class", "word");
        (new qontrol::Row)
            ->margins(resolve(Padding::S), resolve(Padding::XS), resolve(Padding::S),
                      resolve(Padding::XS))
            ->spacing(resolve(Spacing::S))
            ->push(number)
            ->push(word, 1)
            ->into(cell);

        // Column-major: words 1 to `rows` fill the first column.
        m_grid->addWidget(cell, index % rows, index / rows);
    }
}

auto WordGrid::qss(const Palette &p) -> QString {
    return QString("QFrame[class=\"word\"] { background: %1; border: 1px solid %2; "
                   "border-radius: %3px; }")
        .arg(p.surface.name())
        .arg(p.borderLight.name())
        .arg(radius::INPUT);
}

} // namespace catalog
