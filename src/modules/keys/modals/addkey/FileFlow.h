#pragma once

#include <QList>
#include <QString>

#include "AddKeyFlow.h"

#include <silent.h>

class QDragEnterEvent;
class QDropEvent;

namespace catalog {
class Button;
class Card;
class ModalActions;
class Notice;
class ScrollArea;
} // namespace catalog

namespace keys {
class KeysModule;
} // namespace keys

namespace keys::modal {

class KeyReview;

// Adds a key from a key file, dropped on the window or picked, then reviewed before it
// is added with the routes the file names.
class FileFlow final : public AddKeyFlow {
    Q_OBJECT

public:
    explicit FileFlow(KeysModule *module);
    void start() override;

public slots:
    void onBackClicked();
    void onNextClicked();
    void onOpenFileClicked();

protected:
    // The view on display: Back and the primary action act on it.
    enum class View { Drop, Review };

    void init();
    void doConnect();
    void view();

    // Drop view
    void initDrop();
    void connectDrop();
    [[nodiscard]] auto viewDrop() -> catalog::ScrollArea *;
    void showDrop();
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void loadFile(const QString &path);
    void showFileError(const QString &error);

    // Review view
    void initReview();
    void connectReview();
    void showReview();
    void hideReview();

private:
    KeysModule *m_module = nullptr;
    View m_view = View::Drop;

    catalog::Card *m_drop = nullptr;
    catalog::Notice *m_read_error = nullptr;
    catalog::Notice *m_file_error = nullptr;
    catalog::Button *m_open_file = nullptr;
    catalog::ModalActions *m_drop_actions = nullptr;
    catalog::ScrollArea *m_drop_body = nullptr;

    KeyReview *m_review = nullptr;
    QList<KeyRouteKind> m_routes;
};

} // namespace keys::modal
