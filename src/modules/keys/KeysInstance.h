#pragma once

#include <QPointer>
#include <interfaces/host.h>
#include <interfaces/instance.h>

class QEvent;

namespace keys {

class KeysModule;
class KeysTab;

class KeysInstance final : public QObject, public IInstance {
public:
    explicit KeysInstance(KeysModule *module);
    ~KeysInstance() override;

    [[nodiscard]] auto id() const -> QString override;
    void stop() override;
    auto account() -> IAccount * override;
    auto feed() -> IFeed * override;
    auto signer() -> ISigner * override;
    auto theme() -> IThemeProvider * override;
    auto openPage() -> bool;

protected:
    auto eventFilter(QObject *watched, QEvent *event) -> bool override;

private:
    KeysModule *m_module = nullptr;
    QPointer<KeysTab> m_page;
    Host::TabId m_tab_id = 0;
};

} // namespace keys
