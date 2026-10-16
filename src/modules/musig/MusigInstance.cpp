#include "modules/musig/MusigInstance.h"

#include "modules/musig/MusigAccountAdapter.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/MusigAccountWidget.h"

#include <QDebug>
#include <Qontrol>
#include <common.h>
#include <utility>

MusigInstance::MusigInstance(QString id) : m_id(std::move(id)) {
    m_account_widget = new MusigAccountWidget(m_id);
    m_account_controller = m_account_widget->controller();
    m_account = std::make_unique<MusigAccountAdapter>(m_account_controller);

    m_tab_id = Host::get()->openTab(m_account_widget, this);
    if (m_tab_id == 0) {
        qCritical() << "MusigInstance: failed to open tab for account" << m_id;
        m_account_widget->deleteLater();
        m_account_widget = nullptr;
    } else {
        Host::get()->setTabTitle(m_tab_id, m_id);
    }
}

auto MusigInstance::id() const -> QString {
    return m_id;
}

void MusigInstance::stop() {
    if (m_stopped) {
        return;
    }
    m_stopped = true;

    // The tab leaves at once; the widget lives until the controller's background
    // teardown emits stopped().
    if (m_tab_id != 0) {
        Host::get()->closeTab(m_tab_id, false);
        m_tab_id = 0;
    }

    if (m_account_controller != nullptr && m_account_widget != nullptr) {
        QObject::connect(m_account_controller, &MusigAccountController::stopped, m_account_widget,
                         &QWidget::deleteLater, qontrol::UNIQUE);
        m_account_controller->stop();
    } else if (m_account_widget != nullptr) {
        m_account_widget->deleteLater();
    }

    m_account_widget = nullptr;
    m_account_controller = nullptr;
}

auto MusigInstance::account() -> IAccount * {
    return m_account.get();
}

auto MusigInstance::feed() -> IFeed * {
    return nullptr;
}

auto MusigInstance::signer() -> ISigner * {
    return nullptr;
}

auto MusigInstance::theme() -> IThemeProvider * {
    return nullptr;
}
