#pragma once

#include <QMetaType>
#include <QString>

#include <interfaces/account.h>
#include <interfaces/feed.h>
#include <interfaces/signer.h>
#include <interfaces/theme.h>

class IInstance {
public:
    virtual ~IInstance() = default;

    // Register before publishing tabs or settings. Unregistered wizard owners are
    // synchronous: module shutdown must wait until openWizard and its native frames return.

    // Persist this ID, not this live handle. instanceRemoved invalidates the
    // handle; account() is a QObject and can be observed with QPointer.
    [[nodiscard]] virtual QString id() const = 0;
    virtual void stop() = 0;

    virtual IAccount *account() = 0;
    virtual IFeed *feed() = 0;
    virtual ISigner *signer() = 0;
    virtual IThemeProvider *theme() = 0;
};

Q_DECLARE_INTERFACE(IInstance, "dev.silent.IInstance/1.0")

// Not a QObject, so Qt cannot register the pointer on its own: needed for the
// queued hop the host does when a module calls it off the GUI thread.
Q_DECLARE_METATYPE(IInstance *)
