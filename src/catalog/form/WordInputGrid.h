#pragma once

#include <QList>
#include <QStringList>
#include <QWidget>
#include <functional>

class QGridLayout;

namespace catalog {

class ValidatedInput;

// One numbered input per mnemonic word, read down the columns (4 columns above
// 15 words, else 3), each marked valid or invalid by the word check. Pasting a
// whole phrase into any cell spreads it over the following cells.
class WordInputGrid : public QWidget {
    Q_OBJECT

public:
    using WordCheck = std::function<bool(const QString &)>;

    explicit WordInputGrid(QWidget *parent = nullptr);
    // Keeps the words typed in the cells that remain.
    void setWordCount(int count);
    void setWordCheck(WordCheck check);
    [[nodiscard]] auto words() const -> QStringList;

signals:
    void wordsChanged();

public slots:
    void onCellEdited(const QString &text);

protected:
    void markCell(ValidatedInput *cell);

private:
    QGridLayout *m_grid = nullptr;
    QList<ValidatedInput *> m_cells;
    WordCheck m_check;
};

} // namespace catalog
