#pragma once

#include "HwiModule.h"

#include <interfaces/module.h>

class HwiPlugin final : public IPlugin {
public:
    [[nodiscard]] QMap<QString, QString> meta() const override;
    [[nodiscard]] QList<IModule *> modules() const override;

private:
    mutable HwiModule m_module;
};
