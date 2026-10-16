#include "modules/musig/creator/PolicyEditor.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "i18n/Tr.h"
#include "modules/musig/creator/PathEditor.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

namespace musig::modal {

PolicyEditor::PolicyEditor(QWidget *parent) : QWidget(parent) {
    init();
    doConnect();
    view();
    onAddPathClicked();
}

void PolicyEditor::setNetwork(Network network) {
    m_network = network;
}

void PolicyEditor::setKeys(const QList<KeyEntry> &keys) {
    m_keys = keys;
    // Every path holds a row per key: a new key list starts over from one path.
    for (auto *path : m_paths) {
        m_paths_column->remove(path);
    }
    m_paths.clear();
    onAddPathClicked();
}

auto PolicyEditor::spec() const -> PolicySpec {
    PolicySpec out;
    out.network = m_network;
    for (auto *path : m_paths) {
        out.paths.push_back(path->path(m_network));
    }
    return out;
}

void PolicyEditor::init() {
    m_paths_column = new qontrol::Column;
    m_paths_column->spacing(resolve(Spacing::M));
    m_add_path_btn = catalog::btnAddPath();
}

void PolicyEditor::doConnect() {
    connect(m_add_path_btn, &QPushButton::clicked, this, &PolicyEditor::onAddPathClicked,
            qontrol::UNIQUE);
}

void PolicyEditor::view() {
    auto *addRow = (new qontrol::Row)->push(m_add_path_btn)->pushSpacer();
    (new qontrol::Column)
        ->spacing(resolve(Spacing::S))
        ->push(m_paths_column)
        ->push(addRow)
        ->into(this);
}

void PolicyEditor::renumber() {
    for (int i = 0; i < m_paths.size(); ++i) {
        m_paths.at(i)->setTitle(TR("musig-create-path-title").arg(i + 1));
        m_paths.at(i)->setRemovable(m_paths.size() > 1);
    }
}

// ===== Slots =====

void PolicyEditor::onAddPathClicked() {
    auto *path = new PathEditor(m_keys);
    connect(path, &PathEditor::edited, this, &PolicyEditor::onPathEdited, qontrol::UNIQUE);
    connect(path, &PathEditor::removeRequested, this, &PolicyEditor::onPathRemoveRequested,
            qontrol::UNIQUE);
    m_paths.append(path);
    m_paths_column->push(path);
    // A widget added to a shown column stays hidden until the event loop shows it.
    path->show();
    renumber();
    emit edited();
}

void PolicyEditor::onPathEdited() {
    emit edited();
}

void PolicyEditor::onPathRemoveRequested(PathEditor *path) {
    if (!m_paths.removeOne(path)) {
        return;
    }
    // Deferred delete: the path is the emitter.
    m_paths_column->remove(path);
    renumber();
    emit edited();
}

} // namespace musig::modal
