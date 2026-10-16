#include "modules/musig/MusigAccountWidget.h"

MusigAccountWidget::MusigAccountWidget(const QString &account, QWidget *parent) : QWidget(parent) {
    Q_UNUSED(account);
}

MusigAccountWidget::~MusigAccountWidget() = default;

auto MusigAccountWidget::controller() -> MusigAccountController * {
    return m_controller;
}

void MusigAccountWidget::loadPanel(qontrol::Panel *panel) {
    Q_UNUSED(panel);
}

void MusigAccountWidget::initUI() {
}
