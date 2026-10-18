#include "KeyRecord.h"

#include "catalog/containers/Modal.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/KeysView.h"
#include "theme/Palette.h"

#include <QHash>
#include <Qontrol>

#include <algorithm>

namespace keys::modal {

using catalog::Label;
using catalog::LabelRole;
using catalog::LabelTone;

auto keyInfo(KeysModule *module, const QString &fingerprint) -> std::optional<KeyInfo> {
    if (module->rust() == nullptr) {
        return std::nullopt;
    }
    auto result = module->rust()->key(rust::String(fingerprint.toStdString()));
    if (!result.is_ok) {
        return std::nullopt;
    }
    return result.key;
}

auto keyAccounts(KeysModule *module, const QString &fingerprint) -> QList<KeyAccount> {
    // Accounts are told apart by the descriptor they were created with.
    QHash<QString, KeyAccount> configs;
    for (const auto &name : ::list_configs()) {
        auto config = ::config_from_file(name);
        if (config->is_ok()) {
            QString descriptor = KeysModule::toQt(config->get_descriptor());
            configs.insert(descriptor, {.name = KeysModule::toQt(name),
                                        .descriptor = descriptor,
                                        .plugin_id = KeysModule::toQt(config->get_plugin_id())});
        }
    }
    QList<KeyAccount> list;
    for (const auto &descriptor :
         module->rust()->key_descriptors(rust::String(fingerprint.toStdString()))) {
        QString value = KeysModule::toQt(descriptor.descriptor);
        list.append(configs.value(
            value, {.name = KeysModule::toQt(descriptor.alias), .descriptor = value}));
    }
    return list;
}

auto hasRoute(const KeyInfo &info, KeyRouteKind kind) -> bool {
    return std::ranges::find(info.routes, kind) != info.routes.end();
}

auto keyHeader(const QString &title, const QString &fingerprint, const KeyInfo &info) -> QWidget * {
    auto *fingerprintLabel = new Label(fingerprint, LabelRole::Mono);
    fingerprintLabel->setTone(LabelTone::Secondary);
    fingerprintLabel->setSize(catalog::LabelSize::Sm);
    auto *end = (new qontrol::Row)
                    ->spacing(resolve(Spacing::S))
                    ->push(fingerprintLabel)
                    ->push(keyStatusPill(info));
    return catalog::modalHeader(title, end);
}

} // namespace keys::modal
