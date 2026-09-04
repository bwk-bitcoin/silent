#include "HwiPlugin.h"

QMap<QString, QString> HwiPlugin::meta() const {
    return {{QStringLiteral("id"), QStringLiteral("hwi")},
            {QStringLiteral("name"), QStringLiteral("Hardware Wallets")},
            {QStringLiteral("version"), QStringLiteral("1.0.0")}};
}

QList<IModule *> HwiPlugin::modules() const {
    return {&m_module};
}
