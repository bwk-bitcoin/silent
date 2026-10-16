#include "modules/musig/MusigInstance.h"

#include <utility>

MusigInstance::MusigInstance(QString id) : m_id(std::move(id)) {
}

auto MusigInstance::id() const -> QString {
    return m_id;
}

void MusigInstance::stop() {
}

auto MusigInstance::account() -> IAccount * {
    return nullptr;
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
