#include "PluginLibrary.h"
#include "PluginMetadata.h"
#include "interfaces/module.h"

#include <QObject>
#include <QString>
#include <QStringLiteral>
#include <QtCore/qconfig.h>
#include <QtPlugin>
#include <QtTypes>
#include <cstddef>
#include <cstdint>
#include <silent.h>

#ifdef Q_OS_LINUX
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <QFile>
#include <QPluginLoader>
#endif

auto hostArchRequirements() -> quint8 {
    auto requirements = QPluginMetaData::archRequirements();
#if defined(Q_PROCESSOR_X86_64) && (defined(Q_CC_GNU) || defined(Q_CC_CLANG))
    // Test the CPU, not just the instruction set used to compile the host.
    auto level = 1;
    if (__builtin_cpu_supports("x86-64-v4")) {
        level = 4;
    } else if (__builtin_cpu_supports("x86-64-v3")) {
        level = 3;
    } else if (__builtin_cpu_supports("x86-64-v2")) {
        level = 2;
    }
    requirements = (requirements & 0x80) | level;
#endif
    return requirements;
}

auto pluginMetadataError(const QByteArray &bytes, const QString &expected_iid) -> QString {
    auto error = ::plugin_admission(
        rust::Slice<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                                        bytes.size()),
        rust::String(expected_iid.toStdString()), QT_VERSION_MAJOR, QT_VERSION_MINOR,
        hostArchRequirements());
    return QString::fromUtf8(error.data(), error.size());
}

auto installPluginBundle(const QString &path) -> PluginInstallResult {
    return ::plugin_install(rust::String(path.toStdString()), rust::String(SILENT_IPlugin_IID),
                            QT_VERSION_MAJOR, QT_VERSION_MINOR, hostArchRequirements());
}

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
    m_error = pluginMetadataError(bytes, QString::fromLatin1(SILENT_IPlugin_IID));
    if (!m_error.isEmpty()) {
        return false;
    }
    int fd = memfd_create(name.toUtf8().constData(), MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        m_error = QStringLiteral("memfd_create failed");
        return false;
    }
    qsizetype written = 0;
    while (written < bytes.size()) {
        ssize_t n = ::write(fd, bytes.constData() + written,
                            static_cast<std::size_t>(bytes.size() - written));
        if (n <= 0) {
            ::close(fd);
            m_error = QStringLiteral("could not write the library to memory");
            return false;
        }
        written += n;
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
        ::close(fd);
        m_error = QStringLiteral("could not seal the admitted library bytes");
        return false;
    }
    QByteArray path = QStringLiteral("/proc/self/fd/%1").arg(fd).toUtf8();
    m_handle = dlopen(path.constData(), RTLD_NOW | RTLD_LOCAL);
    if (m_handle == nullptr) {
        const char *error = dlerror();
        m_error = QString::fromUtf8(error != nullptr ? error : "dlopen failed");
        ::close(fd);
        return false;
    }
    auto entry = reinterpret_cast<PluginInstanceFn>(dlsym(m_handle, "qt_plugin_instance"));
    if (entry == nullptr) {
        dlclose(m_handle);
        m_handle = nullptr;
        ::close(fd);
        m_error = QStringLiteral("not a Qt plugin: no qt_plugin_instance");
        return false;
    }
    m_instance = entry();
    if (m_instance == nullptr) {
        dlclose(m_handle);
        m_handle = nullptr;
        ::close(fd);
        m_error = QStringLiteral("the plugin created no instance");
        return false;
    }
    // dlopen caches names: reusing /proc/self/fd/N could return another live library.
    m_fd = fd;
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
    ::close(m_fd);
    m_fd = -1;
    return true;
}

#else

auto PluginLibrary::load(const QByteArray &bytes, const QString &name) -> bool {
    m_error = pluginMetadataError(bytes, QString::fromLatin1(SILENT_IPlugin_IID));
    if (!m_error.isEmpty()) {
        return false;
    }
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
