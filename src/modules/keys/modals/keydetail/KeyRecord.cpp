#include "KeyRecord.h"

#include "catalog/containers/Modal.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "modules/keys/KeysModule.h"
#include "modules/keys/KeysView.h"
#include "theme/Palette.h"

#include <QHash>
#include <QRegularExpression>
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

auto hasRoute(const KeyInfo &info, KeyRouteKind kind) -> bool {
    return std::ranges::find(info.routes, kind) != info.routes.end();
}

auto accountXpubs(const QString &fingerprint, Network network) -> QList<AccountXpub> {
    // A key in a sub-account descriptor: [fingerprint/path]xpub, e.g.
    // tr([73c5da0a/86'/1'/0']tpub.../<0;1>/*).
    static const QRegularExpression c_key_origin(
        QStringLiteral(R"(\[([0-9a-fA-F]{8})((?:/[0-9]+['hH]?)+)\]([1-9A-HJ-NP-Za-km-z]+))"));
    QList<AccountXpub> list;
    for (const auto &name : ::list_configs()) {
        auto config = ::config_from_file(name);
        if (!config->is_ok() || config->get_network() != network) {
            continue;
        }
        for (const auto &descriptor :
             {config->get_taproot_descriptor(), config->get_segwit_descriptor()}) {
            auto matches = c_key_origin.globalMatch(KeysModule::toQt(descriptor));
            while (matches.hasNext()) {
                auto match = matches.next();
                if (QString::compare(match.captured(1), fingerprint, Qt::CaseInsensitive) != 0) {
                    continue;
                }
                QString path = match.captured(2).mid(1);
                path.replace(QLatin1Char('h'), QLatin1Char('\''));
                path.replace(QLatin1Char('H'), QLatin1Char('\''));
                AccountXpub entry{.path = path, .xpub = match.captured(3)};
                bool known = std::ranges::any_of(
                    list, [&entry](const AccountXpub &held) { return held.xpub == entry.xpub; });
                if (!known) {
                    list.append(entry);
                }
            }
        }
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
