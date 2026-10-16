#pragma once

#include "interfaces/module.h"
#include "modules/musig/MusigModule.h"
#include "modules/musig/creator/MusigCreatorModule.h"

#include <QList>
#include <QMap>
#include <QString>

class MusigPlugin final : public IPlugin {
public:
    // id "musig", name "MuSig2"
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override;
    // {&m_module, &m_creator}
    [[nodiscard]] auto modules() const -> QList<IModule *> override;

private:
    mutable MusigModule m_module;
    mutable MusigCreatorModule m_creator;
};
