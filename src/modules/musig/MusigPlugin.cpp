#include "modules/musig/MusigPlugin.h"

auto MusigPlugin::meta() const -> QMap<QString, QString> {
    return {{QStringLiteral("id"), QStringLiteral("musig")},
            {QStringLiteral("name"), QStringLiteral("MuSig2")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

auto MusigPlugin::modules() const -> QList<IModule *> {
    return {&m_module, &m_creator};
}
