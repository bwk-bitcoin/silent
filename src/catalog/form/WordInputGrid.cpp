#include "catalog/form/WordInputGrid.h"

#include "catalog/display/Label.h"
#include "catalog/form/ValidatedInput.h"
#include "theme/Palette.h"

#include <QGridLayout>
#include <QRegularExpression>
#include <Qontrol>
#include <common.h>
#include <utility>

namespace catalog {

namespace {

constexpr int COLUMNS = 3;
constexpr int WIDE_COLUMNS = 4;
constexpr int WIDE_ABOVE = 15;
const char *const K_WORD_INDEX = "wordIndex";

} // namespace

WordInputGrid::WordInputGrid(QWidget *parent) : QWidget(parent) {
    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setHorizontalSpacing(resolve(Spacing::S));
    m_grid->setVerticalSpacing(resolve(Spacing::XS));
}

void WordInputGrid::setWordCount(int count) {
    while (auto *item = m_grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_cells.clear();

    int columns = count > WIDE_ABOVE ? WIDE_COLUMNS : COLUMNS;
    int rows = (count + columns - 1) / columns;
    for (int index = 0; index < count; ++index) {
        auto *number = new Label(QString::number(index + 1), LabelRole::Mono);
        number->setSize(LabelSize::Sm);
        number->setTone(LabelTone::Muted);
        number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        number->setFixedWidth(metric::WORD_INDEX_WIDTH);

        auto *cell = new ValidatedInput(InputRole::Mono);
        cell->setProperty(K_WORD_INDEX, index);
        connect(cell, &ValidatedInput::textChanged, this, &WordInputGrid::onCellEdited,
                qontrol::UNIQUE);
        m_cells.append(cell);

        auto *item = (new qontrol::Row)->spacing(resolve(Spacing::XS))->push(number)->push(cell, 1);
        // Column-major: words 1 to `rows` fill the first column.
        m_grid->addWidget(item, index % rows, index / rows);
    }
}

void WordInputGrid::setWordCheck(WordCheck check) {
    m_check = std::move(check);
    for (auto *cell : m_cells) {
        markCell(cell);
    }
}

auto WordInputGrid::words() const -> QStringList {
    QStringList result;
    for (auto *cell : m_cells) {
        result.append(cell->input()->text());
    }
    return result;
}

void WordInputGrid::onCellEdited(const QString &text) {
    auto *cell = qobject_cast<ValidatedInput *>(sender());
    if (cell == nullptr) {
        return;
    }
    static const QRegularExpression c_spaces(QStringLiteral("\\s+"));
    QStringList parts = text.trimmed().split(c_spaces, Qt::SkipEmptyParts);
    if (parts.size() > 1) {
        // A pasted phrase: one word per cell from this one on. Each setText
        // re-enters here with a single word, which marks that cell.
        int start = cell->property(K_WORD_INDEX).toInt();
        for (int offset = 0; offset < parts.size() && start + offset < m_cells.size(); ++offset) {
            m_cells.at(start + offset)->input()->setText(parts.at(offset).toLower());
        }
        return;
    }
    QString word = text.simplified().remove(' ').toLower();
    if (word != text) {
        cell->input()->setText(word);
        return;
    }
    markCell(cell);
    emit wordsChanged();
}

void WordInputGrid::markCell(ValidatedInput *cell) {
    QString word = cell->input()->text();
    if (word.isEmpty() || !m_check) {
        cell->setState(ValidationMark::State::None);
        return;
    }
    cell->setValid(m_check(word));
}

} // namespace catalog
