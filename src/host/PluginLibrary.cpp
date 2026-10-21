#include "PluginLibrary.h"
#include "PluginMetadata.h"
#include "interfaces/module.h"

#include <QCborMap>
#include <QCborStreamReader>
#include <QCborValue>
#include <QtPlugin>
#include <cstddef>
#include <cstring>

#ifdef Q_OS_LINUX
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <QFile>
#include <QPluginLoader>
#endif

auto pluginMetadataError(const QByteArray &bytes, const QString &expected_iid) -> QString {
    // Qt 6.6.3 qplugin.h: ELF note payloads and PE/Mach-O magic headers.
    auto magic = QByteArray(QPluginMetaData::MagicString, sizeof(QPluginMetaData::MagicString));
    auto note = QByteArray(QPluginMetaData::ElfNoteHeader::NoteName,
                           sizeof(QPluginMetaData::ElfNoteHeader::NoteName));
    auto magicOffset = bytes.indexOf(magic);
    auto noteOffset = bytes.indexOf(note);
    if ((magicOffset < 0) == (noteOffset < 0) ||
        (magicOffset >= 0 && bytes.indexOf(magic, magicOffset + 1) >= 0) ||
        (noteOffset >= 0 && bytes.indexOf(note, noteOffset + 1) >= 0)) {
        return QStringLiteral("missing or ambiguous Qt plugin metadata");
    }

    qsizetype headerOffset = magicOffset + magic.size();
    qsizetype endOffset = bytes.size();
    if (noteOffset >= 0) {
        auto noteStart =
            noteOffset - static_cast<qsizetype>(offsetof(QPluginMetaData::ElfNoteHeader, name));
        constexpr auto c_header_offset =
            static_cast<qsizetype>(offsetof(QPluginMetaData::ElfNoteHeader, header));
        if (noteStart < 0 || bytes.size() - noteStart < sizeof(QPluginMetaData::ElfNoteHeader)) {
            return QStringLiteral("truncated Qt plugin metadata note");
        }
        QPluginMetaData::ElfNoteHeader elfNote(0);
        std::memcpy(&elfNote, bytes.constData() + noteStart, sizeof(elfNote));
        headerOffset = noteStart + c_header_offset;
        if (elfNote.n_namesz != sizeof(QPluginMetaData::ElfNoteHeader::NoteName) ||
            elfNote.n_type != QPluginMetaData::ElfNoteHeader::NoteType ||
            elfNote.n_descsz > static_cast<quint64>(bytes.size() - headerOffset)) {
            return QStringLiteral("invalid Qt plugin metadata note");
        }
        endOffset = headerOffset + elfNote.n_descsz;
    }
    if (endOffset - headerOffset <= sizeof(QPluginMetaData::Header)) {
        return QStringLiteral("truncated Qt plugin metadata header");
    }
    QPluginMetaData::Header header;
    std::memcpy(&header, bytes.constData() + headerOffset, sizeof(header));
    if (header.version != QPluginMetaData::CurrentMetaDataVersion ||
        header.qt_major_version != QT_VERSION_MAJOR || header.qt_minor_version > QT_VERSION_MINOR) {
        return QStringLiteral("unsupported Qt plugin metadata version");
    }
    auto payloadOffset = headerOffset + sizeof(header);
    QCborStreamReader reader(reinterpret_cast<const quint8 *>(bytes.constData() + payloadOffset),
                             endOffset - payloadOffset);
    auto metadata = QCborValue::fromCbor(reader);
    if (reader.lastError() != QCborError::NoError || !metadata.isMap()) {
        return QStringLiteral("invalid Qt plugin metadata CBOR");
    }
    // Qt 6.6.3 qplugin_p.h: IID is integer key 2 in the root map, not MetaData.
    constexpr qint64 c_iid_key = 2;
    auto map = metadata.toMap();
    int iidCount = 0;
    QString iid;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (it.key().isInteger() && it.key().toInteger() == c_iid_key) {
            ++iidCount;
            if (!it.value().isString()) {
                return QStringLiteral("invalid Qt plugin root IID");
            }
            iid = it.value().toString();
        }
    }
    if (iidCount != 1 || iid != expected_iid) {
        return QStringLiteral("incompatible Qt plugin root IID (expected %1)").arg(expected_iid);
    }
    return {};
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
