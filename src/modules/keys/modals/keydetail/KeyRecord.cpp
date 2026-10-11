#include "KeyRecord.h"

#include "catalog/containers/Modal.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/KeysView.h"
#include "theme/Palette.h"

#include <QHash>
#include <Qontrol>

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
    QHash<QString, QString> names;
    for (const auto &name : ::list_configs()) {
        auto config = ::config_from_file(name);
        if (config->is_ok()) {
            names.insert(KeysModule::toQt(config->get_descriptor()), KeysModule::toQt(name));
        }
    }
    QList<KeyAccount> list;
    for (const auto &descriptor :
         module->rust()->key_descriptors(rust::String(fingerprint.toStdString()))) {
        QString value = KeysModule::toQt(descriptor.descriptor);
        list.append(
            {.name = names.value(value, KeysModule::toQt(descriptor.alias)), .descriptor = value});
    }
    return list;
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
