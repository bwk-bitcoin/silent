#include "modules/musig/creator/PolicyEditor.h"

namespace musig::modal {

PolicyEditor::PolicyEditor(QWidget *parent) : QWidget(parent) {
}

void PolicyEditor::setNetwork(Network network) {
    Q_UNUSED(network);
}

void PolicyEditor::setKeys(const QList<KeyEntry> &keys) {
    Q_UNUSED(keys);
}

auto PolicyEditor::spec() const -> PolicySpec {
    return {};
}

void PolicyEditor::onAddPathClicked() {
}

void PolicyEditor::onPathEdited() {
}

void PolicyEditor::onPathRemoveRequested(PathEditor *path) {
    Q_UNUSED(path);
}

void PolicyEditor::init() {
}

void PolicyEditor::doConnect() {
}

void PolicyEditor::view() {
}

} // namespace musig::modal
