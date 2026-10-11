#include "Host.h"

#include "AppController.h"
#include "MainWindow.h"
#include "catalog/containers/Modal.h"
#include "i18n/I18nManager.h"
#include "interfaces/instance.h"
#include "interfaces/keys.h"
#include "interfaces/signer.h"

#include <algorithm>

#include <QApplication>
#include <QDebug>
#include <QDialog>
#include <QList>
#include <QThread>
#include <Qontrol>
#include <common.h>

namespace {
Host *gHostSingleton = nullptr;

void assertGuiThread() {
    if (qApp == nullptr) {
        return;
    }
    Q_ASSERT(QThread::currentThread() == qApp->thread());
}

class HostedWizardModal final : public qontrol::Modal {
public:
    explicit HostedWizardModal(QWidget *content) {
        catalog::fixedDialog(this);
        setMainWidget(content);
    }
};

class HostI18nImpl final : public HostI18n {
public:
    auto registerPluginTranslations(const QString &plugin_id, const QString &dir) -> bool override {
        return i18n::I18nManager::get()->registerPluginTranslations(plugin_id, dir);
    }

    void unregisterPluginTranslations(const QString &plugin_id) override {
        i18n::I18nManager::get()->unregisterPluginTranslations(plugin_id);
    }
};
} // namespace

HostImpl::HostImpl() : QObject(nullptr), m_i18n(new HostI18nImpl()) {
    // IInstance is not a QObject, so Qt cannot register the pointer itself.
    qRegisterMetaType<IInstance *>();
    connect(this, &HostImpl::registerInstanceRequested, this,
            &HostImpl::onRegisterInstanceRequested, qontrol::UNIQUE);
    connect(this, &HostImpl::removeInstanceRequested, this, &HostImpl::onRemoveInstanceRequested,
            qontrol::UNIQUE);
    connect(this, &HostImpl::retireInstanceRequested, this, &HostImpl::onRetireInstanceRequested,
            qontrol::UNIQUE);
    connect(this, &HostImpl::setKeysRequested, this, &HostImpl::onSetKeysRequested,
            qontrol::UNIQUE);
    connect(this, &HostImpl::signerPollingRequested, this, &HostImpl::onSignerPollingRequested,
            qontrol::UNIQUE);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
MainWindow *HostImpl::mainWindow() const {
    return dynamic_cast<MainWindow *>(AppController::window());
}

// NOLINTNEXTLINE(misc-no-recursion)
void HostImpl::ensureMainWindowHooked() {
    if (m_main_window_hooked) {
        return;
    }
    auto *window = mainWindow();
    if (window == nullptr) {
        return;
    }
    connect(window, &MainWindow::hostedTabCloseRequested, this,
            &HostImpl::onHostedTabCloseRequested, qontrol::UNIQUE);
    m_main_window_hooked = true;
}

// The three lifecycle entry points are callable from any thread: each one only
// emits, and the self-connection lands the work on the GUI thread.
void HostImpl::registerInstance(const QString &id, IInstance *instance, IModule *owner) {
    emit registerInstanceRequested(id, instance, owner);
}

void HostImpl::removeInstance(const QString &id, IModule *owner) {
    emit removeInstanceRequested(id, owner);
}

void HostImpl::retireInstance(IInstance *self) {
    emit retireInstanceRequested(self);
}

void HostImpl::setKeys(IKeys *keys, IModule *owner) {
    emit setKeysRequested(keys, owner);
}

void HostImpl::requestSignerPolling(IModule *requester, bool enabled) {
    emit signerPollingRequested(requester, enabled);
}

void HostImpl::onRegisterInstanceRequested(const QString &id, IInstance *instance, IModule *owner) {
    assertGuiThread();
    if (instance == nullptr || owner == nullptr) {
        return;
    }
    if (m_instances.contains(id)) {
        qWarning() << "Host: instance id already registered, ignoring:" << id;
        return;
    }
    m_instances.insert(id, instance);
    m_instance_owner.insert(id, owner);
    emit m_events.instanceRegistered(id);
}

void HostImpl::onRemoveInstanceRequested(const QString &id, IModule *owner) {
    assertGuiThread();
    if (owner == nullptr || m_instance_owner.value(id, nullptr) != owner) {
        qWarning() << "Host: refusing to remove an instance the caller does not own:" << id;
        return;
    }
    removeInstance(id);
}

void HostImpl::onRetireInstanceRequested(IInstance *self) {
    assertGuiThread();
    if (self == nullptr) {
        return;
    }
    QString id = self->id();
    if (m_instances.value(id, nullptr) != self) {
        qWarning() << "Host: refusing to retire an unregistered instance:" << id;
        return;
    }
    removeInstance(id);
}

void HostImpl::onSetKeysRequested(IKeys *keys, IModule *owner) {
    assertGuiThread();
    if (keys == nullptr || owner == nullptr) {
        return;
    }
    if (m_keys != nullptr && m_keys_owner != owner) {
        qWarning() << "Host: a keys implementation is already registered, ignoring";
        return;
    }
    m_keys = keys;
    m_keys_owner = owner;
}

void HostImpl::onSignerPollingRequested(IModule *requester, bool enabled) {
    assertGuiThread();
    if (requester == nullptr) {
        qWarning() << "Host: ignoring a signer polling request from no module";
        return;
    }

    if (enabled) {
        m_signer_polling_requests.insert(requester, true);
    } else {
        m_signer_polling_requests.remove(requester);
    }

    bool polling = !m_signer_polling_requests.isEmpty();
    if (polling == m_signer_polling) {
        return;
    }
    m_signer_polling = polling;
    emit m_events.signerPollingChanged(m_signer_polling);
}

QList<IInstance *> HostImpl::instances() const {
    assertGuiThread();
    return m_instances.values();
}

QList<plugin::SignerEntry> HostImpl::signers() const {
    assertGuiThread();
    QList<plugin::SignerEntry> signers;
    for (auto it = m_instances.constBegin(); it != m_instances.constEnd(); ++it) {
        auto *signer = it.value()->signer();
        if (signer == nullptr || !signer->implemented()) {
            continue;
        }
        signers.append(plugin::SignerEntry{
            .instanceId = it.key(),
            .module = m_instance_owner.value(it.key(), nullptr),
            .signer = signer,
        });
    }
    // m_instances is a QHash. Instance ids are unique, so this is a total order and
    // the list keeps the same order between calls.
    std::ranges::sort(signers, [](const plugin::SignerEntry &lhs, const plugin::SignerEntry &rhs) {
        return lhs.instanceId < rhs.instanceId;
    });
    return signers;
}

IKeys *HostImpl::keys() const {
    assertGuiThread();
    return m_keys;
}

// NOLINTNEXTLINE(misc-no-recursion)
Host::TabId HostImpl::openTab(QWidget *content, IInstance *owner) {
    assertGuiThread();
    if (content == nullptr || owner == nullptr) {
        qWarning() << "Host openTab failed: null content or owner";
        return 0;
    }
    ensureMainWindowHooked();
    auto *window = mainWindow();
    if (window == nullptr) {
        qWarning() << "Host openTab failed: MainWindow unavailable";
        return 0;
    }

    if (m_tab_by_owner.contains(owner)) {
        // This owner already has a hosted tab; host does not take ownership of replacement content.
        // Dispose the extra widget to avoid ambiguous ownership/lifecycle leaks.
        content->deleteLater();
        return m_tab_by_owner.value(owner);
    }

    const TabId tab_id = ++m_next_tab_id;
    window->addTab(content, QString());
    m_tabs.insert(tab_id, TabEntry{.id = tab_id, .content = content, .owner = owner});
    m_tab_by_owner.insert(owner, tab_id);
    return tab_id;
}

void HostImpl::setTabTitle(TabId tab, const QString &title) {
    assertGuiThread();
    if (!m_tabs.contains(tab)) {
        return;
    }
    auto *window = mainWindow();
    if (window == nullptr) {
        return;
    }
    auto &entry = m_tabs[tab];
    window->setTabTitle(entry.content, title);
}

void HostImpl::closeTab(TabId tab, bool delete_content) {
    assertGuiThread();
    if (!m_tabs.contains(tab)) {
        return;
    }
    auto entry = m_tabs.take(tab);
    m_tab_by_owner.remove(entry.owner);
    auto *window = mainWindow();
    if (window != nullptr) {
        window->removeTab(entry.content, delete_content);
    } else if (entry.content != nullptr && delete_content) {
        entry.content->deleteLater();
    }
}

Host::SectionId HostImpl::addSettingSection(QWidget *content, IInstance *owner,
                                            const QString &title) {
    assertGuiThread();
    if (content == nullptr || owner == nullptr) {
        return 0;
    }

    const SectionId section_id = ++m_next_section_id;
    m_sections.insert(section_id, SectionEntry{
                                      .id = section_id,
                                      .content = content,
                                      .owner = owner,
                                      .title = title.trimmed(),
                                  });
    emit m_events.settingSectionsChanged();
    return section_id;
}

void HostImpl::delSettingSection(SectionId section) {
    assertGuiThread();
    if (!m_sections.contains(section)) {
        return;
    }
    auto entry = m_sections.take(section);
    if (entry.content != nullptr) {
        entry.content->deleteLater();
    }
    emit m_events.settingSectionsChanged();
}

Host::WizardId HostImpl::openWizard(QWidget *content, IInstance *owner) {
    assertGuiThread();
    if (content == nullptr || owner == nullptr) {
        return 0;
    }

    const WizardId wizard_id = ++m_next_wizard_id;
    auto *modal = new HostedWizardModal(content);
    m_wizards.insert(
        wizard_id,
        WizardEntry{.id = wizard_id, .content = content, .modal = modal, .owner = owner});
    m_wizard_by_modal.insert(modal, wizard_id);
    connect(modal, &QDialog::finished, this, &HostImpl::onWizardModalFinished, qontrol::UNIQUE);
    AppController::execModal(modal);
    return wizard_id;
}

void HostImpl::closeWizard(WizardId wizard) {
    assertGuiThread();
    if (!m_wizards.contains(wizard)) {
        return;
    }

    auto entry = m_wizards.take(wizard);
    if (entry.modal != nullptr) {
        m_wizard_by_modal.remove(entry.modal);
        auto *dialog = qobject_cast<QDialog *>(entry.modal.data());
        if (dialog != nullptr && dialog->isVisible()) {
            dialog->reject();
        }
        entry.modal->deleteLater();
    }
    if (entry.content != nullptr) {
        entry.content->deleteLater();
    }
}

HostI18n *HostImpl::i18n() {
    return m_i18n;
}

HostEvents *HostImpl::events() {
    return &m_events;
}

void HostImpl::onInstanceStopped(const QString &id) {
    assertGuiThread();
    auto *instance = m_instances.value(id, nullptr);
    onInstanceStopped(instance);
}

void HostImpl::onInstanceStopped(IInstance *instance) {
    assertGuiThread();
    if (instance == nullptr) {
        return;
    }
    closeOwnedUi(instance);
}

void HostImpl::removeInstance(const QString &id) {
    assertGuiThread();
    if (!m_instances.contains(id)) {
        return;
    }
    auto *instance = m_instances.value(id);
    instance->stop();
    closeOwnedUi(instance);
    m_instances.remove(id);
    m_instance_owner.remove(id);
    // Emitted last: a module frees its own instance from this signal.
    emit m_events.instanceRemoved(id);
}

IModule *HostImpl::instanceOwner(const QString &id) const {
    assertGuiThread();
    return m_instance_owner.value(id, nullptr);
}

QList<Host::SettingSectionView> HostImpl::settingSections() const {
    assertGuiThread();
    QList<Host::SettingSectionView> sections;
    sections.reserve(m_sections.size());
    for (const auto &entry : m_sections) {
        if (entry.content != nullptr) {
            sections.append(Host::SettingSectionView{
                .id = entry.id,
                .content = entry.content.data(),
                .title = entry.title,
            });
        }
    }
    return sections;
}

auto HostImpl::settingSectionModule(SectionId section) const -> IModule * {
    assertGuiThread();
    auto *owner = m_sections.value(section).owner;
    if (owner == nullptr) {
        return nullptr;
    }
    return m_instance_owner.value(owner->id(), nullptr);
}

void HostImpl::onHostedTabCloseRequested(QWidget *content) {
    assertGuiThread();
    IInstance *owner = nullptr;
    for (auto it = m_tabs.constBegin(); it != m_tabs.constEnd(); ++it) {
        if (it.value().content == content) {
            owner = it.value().owner;
            break;
        }
    }

    if (owner != nullptr) {
        // Closing the tab tears the instance down completely: removeInstance stops it
        // and fires instanceRemoved (clearing open-account state and letting the owning
        // module recycle the id). Without the removal the instance lingers registered
        // and cannot be reopened from the launcher.
        removeInstance(owner->id());
    }
}

void HostImpl::onWizardModalFinished(int result) {
    Q_UNUSED(result);
    assertGuiThread();
    auto *dialog = qobject_cast<QDialog *>(sender());
    if (dialog == nullptr || !m_wizard_by_modal.contains(dialog)) {
        return;
    }
    const WizardId wizard_id = m_wizard_by_modal.value(dialog);
    m_wizard_by_modal.remove(dialog);
    if (!m_wizards.contains(wizard_id)) {
        return;
    }
    auto entry = m_wizards.take(wizard_id);
    if (entry.content != nullptr) {
        entry.content->deleteLater();
    }
    dialog->deleteLater();
}

void HostImpl::closeOwnedUi(IInstance *owner) {
    if (owner == nullptr) {
        return;
    }

    if (m_tab_by_owner.contains(owner)) {
        closeTab(m_tab_by_owner.value(owner));
    }

    QList<SectionId> sectionIds;
    for (auto it = m_sections.constBegin(); it != m_sections.constEnd(); ++it) {
        if (it.value().owner == owner) {
            sectionIds.append(it.key());
        }
    }
    for (SectionId id : sectionIds) {
        delSettingSection(id);
    }

    QList<WizardId> wizardIds;
    for (auto it = m_wizards.constBegin(); it != m_wizards.constEnd(); ++it) {
        if (it.value().owner == owner) {
            wizardIds.append(it.key());
        }
    }
    for (WizardId id : wizardIds) {
        closeWizard(id);
    }
}

Host *Host::get() {
    if (gHostSingleton == nullptr) {
        gHostSingleton = new HostImpl();
    }
    return gHostSingleton;
}
