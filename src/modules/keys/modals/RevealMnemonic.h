#pragma once

#include "catalog/containers/Modal.h"

#include <QStringList>

namespace catalog {
class Button;
class WordGrid;
} // namespace catalog

namespace keys::modal {

// A hot key's mnemonic, unsealed: the words under a warning, with a toggle to mask
// them. Opened over the key detail once the password opened the key.
class RevealMnemonic final : public catalog::Modal {
    Q_OBJECT

public:
    explicit RevealMnemonic(const QStringList &words);
    ~RevealMnemonic() override;

public slots:
    void onToggleWordsClicked();

protected:
    void init(const QStringList &words);
    void doConnect();
    void view();

private:
    catalog::WordGrid *m_words = nullptr;
    catalog::Button *m_hide_words = nullptr;
    catalog::Button *m_show_words = nullptr;
    catalog::Button *m_done = nullptr;
};

} // namespace keys::modal
