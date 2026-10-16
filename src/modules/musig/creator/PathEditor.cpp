#include "modules/musig/creator/PathEditor.h"

namespace musig::modal {

PathEditor::PathEditor(const QList<KeyEntry> &keys, QWidget *parent) : QWidget(parent) {
    Q_UNUSED(keys);
}

auto PathEditor::path(Network network) const -> PolicyPath {
    Q_UNUSED(network);
    return {};
}

void PathEditor::onSemanticChanged(int index) {
    Q_UNUSED(index);
}

void PathEditor::onThresholdChanged(int value) {
    Q_UNUSED(value);
}

void PathEditor::onLocktimeKindChanged(int index) {
    Q_UNUSED(index);
}

void PathEditor::onLocktimeValueChanged(int value) {
    Q_UNUSED(value);
}

void PathEditor::onKeyToggled(bool checked) {
    Q_UNUSED(checked);
}

void PathEditor::onKeyTypeChanged(int index) {
    Q_UNUSED(index);
}

void PathEditor::onAccountChanged(int value) {
    Q_UNUSED(value);
}

void PathEditor::onMandatoryToggled(bool checked) {
    Q_UNUSED(checked);
}

void PathEditor::onRemoveClicked() {
}

void PathEditor::init() {
}

void PathEditor::doConnect() {
}

void PathEditor::view() {
}

} // namespace musig::modal
