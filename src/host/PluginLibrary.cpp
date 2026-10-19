#include "PluginLibrary.h"

#ifdef Q_OS_LINUX
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <QFile>
#include <QPluginLoader>
#endif

PluginLibrary::~PluginLibrary() {
    unload();
}

auto PluginLibrary::instance() const -> QObject * {
    return m_instance.data();
}

auto PluginLibrary::errorString() const -> QString {
    return m_error;
}

#ifdef Q_OS_LINUX

namespace {
// The entry point Q_PLUGIN_METADATA exports: the plugin's root object, created once.
using PluginInstanceFn = QObject *(*)();
} // namespace

auto PluginLibrary::load(const QByteArray &bytes, const QString &name) -> bool {
    m_error.clear();
    int fd = memfd_create(name.toUtf8().constData(), MFD_CLOEXEC);
    if (fd < 0) {
        m_error = QStringLiteral("memfd_create failed");
        return false;
    }
    qsizetype written = 0;
    while (written < bytes.size()) {
        ssize_t n =
            ::write(fd, bytes.constData() + written, static_cast<size_t>(bytes.size() - written));
        if (n <= 0) {
            ::close(fd);
            m_error = QStringLiteral("could not write the library to memory");
            return false;
        }
        written += n;
    }
    QByteArray path = QStringLiteral("/proc/self/fd/%1").arg(fd).toUtf8();
    m_handle = dlopen(path.constData(), RTLD_NOW | RTLD_LOCAL);
    // The mapping keeps the bytes: the descriptor is no longer needed.
    ::close(fd);
    if (m_handle == nullptr) {
        const char *error = dlerror();
        m_error = QString::fromUtf8(error != nullptr ? error : "dlopen failed");
        return false;
    }
    auto entry = reinterpret_cast<PluginInstanceFn>(dlsym(m_handle, "qt_plugin_instance"));
    if (entry == nullptr) {
        dlclose(m_handle);
        m_handle = nullptr;
        m_error = QStringLiteral("not a Qt plugin: no qt_plugin_instance");
        return false;
    }
    m_instance = entry();
    if (m_instance == nullptr) {
        dlclose(m_handle);
        m_handle = nullptr;
        m_error = QStringLiteral("the plugin created no instance");
        return false;
    }
    return true;
}

auto PluginLibrary::unload() -> bool {
    if (m_handle == nullptr) {
        return true;
    }
    // Its code lives in the library: the root object goes first.
    delete m_instance.data();
    if (dlclose(m_handle) != 0) {
        const char *error = dlerror();
        m_error = QString::fromUtf8(error != nullptr ? error : "dlclose failed");
        return false;
    }
    m_handle = nullptr;
    return true;
}

#else

auto PluginLibrary::load(const QByteArray &bytes, const QString &name) -> bool {
    m_error.clear();
    m_dir = new QTemporaryDir();
    if (!m_dir->isValid()) {
        m_error = m_dir->errorString();
        delete m_dir;
        m_dir = nullptr;
        return false;
    }
    m_path = m_dir->filePath(name);
    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        m_error = file.errorString();
        return false;
    }
    file.close();
    m_loader = new QPluginLoader(m_path);
    m_instance = m_loader->instance();
    if (m_instance == nullptr) {
        m_error = m_loader->errorString();
        delete m_loader;
        m_loader = nullptr;
        delete m_dir;
        m_dir = nullptr;
        return false;
    }
    // Loaded: the file is no longer needed where the system lets it go.
    QFile::remove(m_path);
    return true;
}

auto PluginLibrary::unload() -> bool {
    if (m_loader == nullptr) {
        return true;
    }
    if (!m_loader->unload()) {
        m_error = m_loader->errorString();
        return false;
    }
    delete m_loader;
    m_loader = nullptr;
    delete m_dir;
    m_dir = nullptr;
    return true;
}

#endif
