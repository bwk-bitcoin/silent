#pragma once

#include <QList>
#include <QString>
#include <cstdint>
#include <qevent.h>

#include "catalog/containers/Modal.h"
#include "host/PluginRegistry.h"

namespace catalog {
class ModalActions;
class SelectRow;
} // namespace catalog

namespace modal {

// The account types of the launcher: the built-in ones, then those of the enabled plugins.
// Picking a type offers Create and Restore when it has both, else starts its one entry. A
// started entry closes the modal.
class AccountType : public catalog::Modal {
    Q_OBJECT

public:
    explicit AccountType(const QList<PluginRegistry::CreatorLauncher> &types);

public slots:
    void onBackClicked();
    void onTypeClicked();
    void onCreateClicked();
    void onRestoreClicked();

protected:
    enum class View : std::uint8_t { Types, Choice };

    void init();
    void doConnect();
    void view();
    void changeEvent(QEvent *event) override;
    void retranslateUi();

    // Types view
    void initTypes();
    void connectTypes();
    auto viewTypes() -> QWidget *;
    void showTypes();

    // Choice view
    void initChoice();
    void connectChoice();
    auto viewChoice() -> QWidget *;
    void showChoice();
    void hideChoice();

    // Shared
    // The window title of the view on display.
    [[nodiscard]] auto title() const -> QString;
    // Closes the modal and starts the entry `id` of the picked type's module.
    void startEntry(const QString &id);

private:
    QList<PluginRegistry::CreatorLauncher> m_types;
    View m_view = View::Types;
    // The type the choice is for.
    int m_type = 0;

    QWidget *m_types_view = nullptr;
    QList<catalog::SelectRow *> m_type_rows;

    QWidget *m_choice_view = nullptr;
    catalog::SelectRow *m_create_row = nullptr;
    catalog::SelectRow *m_restore_row = nullptr;
    catalog::ModalActions *m_choice_actions = nullptr;
};

} // namespace modal
