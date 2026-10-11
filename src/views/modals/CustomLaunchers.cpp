#include "CustomLaunchers.h"
#include "../utils.h"
#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/containers/Modal.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "interfaces/module.h"
#include <QVariant>
#include <Qontrol>
#include <common.h>

namespace modal {

using catalog::Button;
using catalog::ButtonRole;

CustomLaunchers::CustomLaunchers([[maybe_unused]] QWidget *parent) {
    catalog::fixedDialog(this);
    setWindowTitle(TR("menu-custom"));
    init();
    doConnect();
    view();
}

void CustomLaunchers::init() {
}

void CustomLaunchers::doConnect() {
}

void CustomLaunchers::onCreatorClicked() {
    auto *btn = qobject_cast<QPushButton *>(sender());
    if (btn == nullptr) {
        return;
    }
    auto id = btn->property("instanceId").toString();
    auto modulePtr = btn->property("modulePtr").toULongLong();
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *module = reinterpret_cast<IModule *>(modulePtr);
    if (module != nullptr && !id.isEmpty()) {
        // Route through the registry so the created instance is attributed to its
        // owning plugin (required for clean disable later).
        auto *controller = AppController::get();
        auto *registry = controller != nullptr ? controller->pluginRegistry() : nullptr;
        if (registry != nullptr) {
            registry->startLauncherInstance(module, id);
        } else {
            module->startInstance(id);
        }
        accept();
    }
}

void CustomLaunchers::view() {
    auto *col = (new qontrol::Column)->pushSpacer(resolve(Spacing::M));

    auto *controller = AppController::get();
    if (controller != nullptr && controller->pluginRegistry() != nullptr) {
        auto creators = controller->pluginRegistry()->externalCreatorLaunchers();
        for (const auto &creator : creators) {
            auto *btn = new Button(creator.name, ButtonRole::TabCreate);
            btn->setProperty("instanceId", creator.id);
            btn->setProperty("modulePtr",
                             QVariant::fromValue(reinterpret_cast<qulonglong>(creator.module)));
            connect(btn, &QPushButton::clicked, this, &CustomLaunchers::onCreatorClicked,
                    qontrol::UNIQUE);
            col->push((new qontrol::Row)->pushSpacer()->push(btn)->pushSpacer())
                ->pushSpacer(resolve(Spacing::S));
        }
    }

    col->pushSpacer();

    setMainWidget(margin(col));
}

void CustomLaunchers::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    qontrol::Modal::changeEvent(event);
}

void CustomLaunchers::retranslateUi() {
    setWindowTitle(TR("menu-custom"));
}

} // namespace modal
