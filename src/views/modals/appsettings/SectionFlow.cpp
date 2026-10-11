#include "SectionFlow.h"

#include "catalog/Button.h"
#include "catalog/Variants.h"
#include "catalog/containers/Card.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/containers/ScrollArea.h"
#include "catalog/containers/ViewSlots.h"
#include "catalog/display/Label.h"
#include "theme/Palette.h"

#include <Qontrol>
#include <common.h>

#include <utility>

namespace modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

SectionFlow::SectionFlow(Host *host, Host::SectionId section_id, QString title, const QString &text,
                         QWidget *content)
    : m_host(host),
      m_title(std::move(title)),
      m_open_section(content),
      m_open_section_id(section_id) {
    init(text);
    doConnect();
    view();
}

SectionFlow::~SectionFlow() {
    // Hand the module its widget back before the slot and its children go.
    if (m_open_section != nullptr) {
        m_section_slot->take(m_open_section.data());
    }
}

void SectionFlow::start() {
    m_section_slot->push(m_open_section.data());
    m_open_section->show();
    showSection();
}

void SectionFlow::init(const QString &text) {
    initSection(text);
}

void SectionFlow::doConnect() {
    connectSection();
}

void SectionFlow::view() {
    m_section = viewSection();
}

// ===== Section view =====

void SectionFlow::initSection(const QString &text) {
    m_section_text = new Label(LabelRole::Body);
    m_section_text->setWordWrap(true);
    m_section_text->setTone(LabelTone::Secondary);
    m_section_text->setText(text);
    m_section_slot = new qontrol::Column;
    m_section_actions = new catalog::ModalActions;
    m_section_actions->setBack(catalog::ModalActions::Back::AllSettings);
    m_done_btn = catalog::btnDone();
    m_section_actions->addAction(m_done_btn);
}

void SectionFlow::connectSection() {
    connect(m_done_btn, &QPushButton::clicked, this, &SectionFlow::onNextClicked, qontrol::UNIQUE);
    connect(m_section_actions, &catalog::ModalActions::backClicked, this,
            &SectionFlow::onBackClicked, qontrol::UNIQUE);
    if (m_host != nullptr) {
        connect(m_host->events(), &HostEvents::settingSectionsChanged, this,
                &SectionFlow::onHostSettingsSectionsChanged, qontrol::UNIQUE);
    }
}

auto SectionFlow::viewSection() -> catalog::ScrollArea * {
    auto *card = new catalog::Card(catalog::Card::Role::Inset);
    card->setPad(catalog::Card::Pad::M);
    card->setContent(m_section_slot);
    auto *column =
        (new qontrol::Column)->spacing(resolve(Spacing::S))->push(m_section_text)->push(card);
    auto *scroll = new catalog::ScrollArea;
    scroll->setFitContent(true);
    scroll->setWidget(column);
    return scroll;
}

void SectionFlow::showSection() {
    viewSlots()->setHeader(nullptr);
    viewSlots()->setBody(m_section);
    viewSlots()->setFooter(m_section_actions);
    emit viewChanged(m_title, ModalWidth::Wide);
}

void SectionFlow::closeSection() {
    if (m_open_section != nullptr) {
        m_section_slot->take(m_open_section.data());
    }
    m_open_section = nullptr;
    m_open_section_id = 0;
    emit back();
}

// ===== Slots =====

void SectionFlow::onBackClicked() {
    closeSection();
}

void SectionFlow::onNextClicked() {
    closeSection();
}

void SectionFlow::onHostSettingsSectionsChanged() {
    if (m_open_section_id == 0) {
        return;
    }
    for (const auto &entry : m_host->settingSections()) {
        if (entry.id == m_open_section_id) {
            return;
        }
    }
    // The module dropped the section on display.
    closeSection();
}

} // namespace modal
