#pragma once

#include "interfaces/keys.h"

#include <QList>
#include <QWidget>
#include <silent.h>

namespace catalog {
class Button;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig::modal {

class PathEditor;

// Step 1: a list of PathEditor.
class PolicyEditor : public QWidget {
    Q_OBJECT

public:
    explicit PolicyEditor(QWidget *parent = nullptr);
    void setNetwork(Network network);
    void setKeys(const QList<KeyEntry> &keys);
    // account_xpub empty on every key until step 3 fills it.
    [[nodiscard]] auto spec() const -> PolicySpec;

signals:
    void edited();

public slots:
    void onAddPathClicked();
    void onPathEdited();
    void onPathRemoveRequested(PathEditor *path);

protected:
    void init();
    void doConnect();
    void view();

private:
    Network m_network = Network::Regtest;
    QList<KeyEntry> m_keys;
    QList<PathEditor *> m_paths;
    qontrol::Column *m_paths_column = nullptr;
    catalog::Button *m_add_path_btn = nullptr;
};

} // namespace musig::modal
