#pragma once

#include "theme/Palette.h"
#include <QStringList>
#include <QWidget>

class QGridLayout;

namespace catalog {

// Numbered mnemonic words read down the columns: 3 columns, 4 above 12 words.
// Masked, the words show as dots and the layout stays.
class WordGrid : public QWidget {
    Q_OBJECT

public:
    explicit WordGrid(QWidget *parent = nullptr);
    void setWords(const QStringList &words);
    void setMasked(bool masked);
    static auto qss(const Palette &p) -> QString;

protected:
    void rebuild();

private:
    QGridLayout *m_grid = nullptr;
    QStringList m_words;
    bool m_masked = false;
};

} // namespace catalog
