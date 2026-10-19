#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#ifndef Q_OS_LINUX
#include <QTemporaryDir>
class QPluginLoader;
#endif

// An external plugin's library, loaded from bytes held in memory: the bytes opened
// from the app state, the ones the host hashed (ws-plaude/silent#41). Never from a
// path another process could swap.
//
// Linux: the bytes go to an anonymous memory file (memfd_create) and the library is
// opened from /proc/self/fd/N, its Qt entry point called by hand. macOS and Windows:
// a temporary file in a private directory, removed once loaded (Windows keeps a
// loaded library's file: it goes once the library is unloaded).
class PluginLibrary final {
public:
    PluginLibrary() = default;
    ~PluginLibrary();
    PluginLibrary(const PluginLibrary &) = delete;
    auto operator=(const PluginLibrary &) -> PluginLibrary & = delete;
    PluginLibrary(PluginLibrary &&) = delete;
    auto operator=(PluginLibrary &&) -> PluginLibrary & = delete;

    // Loads `bytes`, a library named `name` (for its type), and creates its root
    // object. False with errorString() set when it does not load.
    auto load(const QByteArray &bytes, const QString &name) -> bool;
    // The plugin's root object, null until loaded.
    [[nodiscard]] auto instance() const -> QObject *;
    // Deletes the root object, then unloads the library. False when it stays loaded.
    auto unload() -> bool;
    [[nodiscard]] auto errorString() const -> QString;

private:
    QString m_error;
    QPointer<QObject> m_instance;
#ifdef Q_OS_LINUX
    void *m_handle = nullptr;
#else
    QTemporaryDir *m_dir = nullptr;
    QPluginLoader *m_loader = nullptr;
    QString m_path;
#endif
};
