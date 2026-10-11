#include "AddKey.h"

#include "modules/keys/modals/addkey/FileFlow.h"
#include "modules/keys/modals/addkey/HotKeyFlow.h"
#include "modules/keys/modals/addkey/PasteFlow.h"
#include "modules/keys/modals/addkey/QrFlow.h"

#include "catalog/SelectRow.h"
#include "catalog/containers/ViewSlots.h"
#include "i18n/Tr.h"
#include "theme/Icon.h"
#include "theme/Palette.h"

#include <common.h>

namespace keys::modal {

namespace {

const char *const K_MODE = "addKeyMode";

enum class Mode { Qr, File, Paste, Hot };

auto modeRow(Mode mode, IconFn icon, const QString &title, const QString &sub)
    -> catalog::SelectRow * {
    auto *row = new catalog::SelectRow;
    row->setIcon(icon);
    row->setTitle(title);
    row->setSub(sub);
    row->setProperty(K_MODE, static_cast<int>(mode));
    return row;
}

} // namespace

AddKey::AddKey(KeysModule *module) : m_module(module) {
    init();
    doConnect();
    view();
    showModes();
}

void AddKey::done(int result) {
    closeFlow();
    catalog::Modal::done(result);
}

void AddKey::init() {
    initModes();
}

void AddKey::doConnect() {
    connectModes();
}

void AddKey::view() {
    m_modes_body = viewModes();
}

// ===== Modes view =====

void AddKey::initModes() {
    m_mode_rows = {
        modeRow(Mode::Qr, icon::qr, TR("keys-add-mode-qr"), TR("keys-add-mode-qr-sub")),
        modeRow(Mode::File, icon::folder, TR("keys-add-mode-file"), TR("keys-add-mode-file-sub")),
        modeRow(Mode::Paste, icon::pencil, TR("keys-add-mode-paste"),
                TR("keys-add-mode-paste-sub")),
        modeRow(Mode::Hot, icon::key, TR("keys-add-mode-hot"), TR("keys-add-mode-hot-sub")),
    };
}

void AddKey::connectModes() {
    for (auto *row : m_mode_rows) {
        connect(row, &catalog::SelectRow::clicked, this, &AddKey::onModeClicked, qontrol::UNIQUE);
    }
}

auto AddKey::viewModes() -> QWidget * {
    auto *column = (new qontrol::Column)->spacing(resolve(Spacing::S));
    for (auto *row : m_mode_rows) {
        column->push(row);
    }
    return column;
}

void AddKey::showModes() {
    setWindowTitle(TR("keys-add-title"));
    // The rows span the modal, as in the design, rather than centred at their width.
    setPage(m_modes_body);
    setWidth(ModalWidth::L);
}

// ===== Shared =====

void AddKey::closeFlow() {
    if (m_flow == nullptr) {
        return;
    }
    // Deleted later: the flow may still be in the slot that emitted.
    m_flow->deleteLater();
    m_flow = nullptr;
}

// ===== Slots =====

void AddKey::onModeClicked() {
    switch (static_cast<Mode>(sender()->property(K_MODE).toInt())) {
    case Mode::Qr:
        m_flow = new QrFlow(m_module);
        break;
    case Mode::File:
        m_flow = new FileFlow(m_module);
        break;
    case Mode::Paste:
        m_flow = new PasteFlow(m_module);
        break;
    case Mode::Hot:
        m_flow = new HotKeyFlow(m_module);
        break;
    }
    connect(m_flow, &AddKeyFlow::viewChanged, this, &AddKey::onFlowViewChanged, qontrol::UNIQUE);
    connect(m_flow, &AddKeyFlow::back, this, &AddKey::onFlowBack, qontrol::UNIQUE);
    connect(m_flow, &AddKeyFlow::added, this, &AddKey::accept, qontrol::UNIQUE);
    setPage(m_flow);
    m_flow->start();
}

void AddKey::onFlowViewChanged(const QString &title, ModalWidth width) {
    setWindowTitle(title);
    setWidth(width);
}

void AddKey::onFlowBack() {
    showModes();
    closeFlow();
}

// ===== AddKeyFlow =====

AddKeyFlow::AddKeyFlow(QWidget *parent) : QWidget(parent) {
    m_slots = new catalog::ViewSlots;
    (new qontrol::Column)->push(m_slots)->into(this);
}

auto AddKeyFlow::viewSlots() const -> catalog::ViewSlots * {
    return m_slots;
}

} // namespace keys::modal
