#pragma once

#include <QList>
#include <QString>

#include "catalog/containers/ScrollArea.h"
#include "modules/keys/KeysModule.h"

namespace catalog {
class Button;
class Input;
class Label;
class ModalActions;
class Notice;
class Table;
} // namespace catalog

namespace keys::modal {

// Reviews a key read off a device or a file before it is added: its xpubs, a notice
// when the key is already registered, and the alias to give it. The owner puts
// header() and actions() around it.
class KeyReview : public catalog::ScrollArea {
    Q_OBJECT

public:
    KeyReview(KeysModule *module, const QString &title);
    // Shows the key with `alias` in the alias field.
    void setKey(const QString &fingerprint, const QList<PathXpub> &xpubs, const QString &alias);
    // Adds the key shown, signing over `routes`. Shows why when the module refuses it.
    [[nodiscard]] auto add(const QList<KeyRouteKind> &routes) -> bool;
    // The title, with the key's fingerprint on its right.
    [[nodiscard]] auto header() const -> QWidget *;
    // Back, then Add Key with why it is disabled.
    [[nodiscard]] auto actions() const -> catalog::ModalActions *;

signals:
    void backClicked();
    void addClicked();

public slots:
    void onAliasEdited();
    void onTableRebuilt();

protected:
    void init(const QString &title);
    void doConnect();
    void view();

private:
    KeysModule *m_module = nullptr;
    QString m_fingerprint;
    QList<PathXpub> m_xpubs;
    QWidget *m_header = nullptr;
    catalog::Label *m_fingerprint_label = nullptr;
    catalog::Table *m_table = nullptr;
    catalog::Notice *m_duplicate = nullptr;
    QWidget *m_alias_field = nullptr;
    catalog::Input *m_alias = nullptr;
    catalog::Notice *m_error = nullptr;
    catalog::Button *m_add = nullptr;
    catalog::ModalActions *m_actions = nullptr;
};

} // namespace keys::modal
