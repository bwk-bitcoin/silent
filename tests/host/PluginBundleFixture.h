#pragma once

// Plugin bundles for the host smokes: a stored ZIP holding manifest.json and one native
// library under payload/, imported through the host's real importer. Linux fixtures only.

#include "host/PluginMetadata.h"
#include "interfaces/module.h"
#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QPluginLoader>
#include <QString>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtLogging>
#include <QtTypes>
#include <silent.h>

namespace smoke {

// The names Rust's std::env::consts gives this build's target.
inline constexpr auto BUNDLE_TARGET_OS = "linux";
#ifdef Q_PROCESSOR_ARM_64
inline constexpr auto BUNDLE_TARGET_ARCH = "aarch64";
#else
inline constexpr auto BUNDLE_TARGET_ARCH = "x86_64";
#endif

// The Qt host a manifest declares its native library is built for.
struct BundleCompatibility {
    QString iid;
    int qtMajor = 0;
    int qtMinor = 0;
    int archRequirements = 0;
};

// This host, as the Settings import passes it.
inline auto hostCompatibility() -> BundleCompatibility {
    return {.iid = QStringLiteral(SILENT_IPlugin_IID),
            .qtMajor = QT_VERSION_MAJOR,
            .qtMinor = QT_VERSION_MINOR,
            .archRequirements = hostArchRequirements()};
}

// What the library's own Qt metadata declares, read by Qt without loading it.
inline auto libraryCompatibility(const QByteArray &library) -> BundleCompatibility {
    QTemporaryDir directory;
    QFile file(directory.filePath(QStringLiteral("libcompatibility.so")));
    if (!directory.isValid() || !file.open(QIODevice::WriteOnly) ||
        file.write(library) != library.size()) {
        qFatal("plugin bundle fixture: the library copy for its metadata failed");
    }
    file.close();
    auto metadata = QPluginLoader(file.fileName()).metaData();
    if (metadata.isEmpty()) {
        qFatal("plugin bundle fixture: the library has no Qt metadata");
    }
    // Qt reports the header's Qt version as major << 16 | minor << 8, and its CPU byte
    // as the level and the debug flag apart.
    auto version = metadata.value(QStringLiteral("version")).toInt();
    auto debug = metadata.value(QStringLiteral("debug")).toBool() ? 0x80 : 0;
    return {.iid = metadata.value(QStringLiteral("IID")).toString(),
            .qtMajor = (version >> 16) & 0xff,
            .qtMinor = (version >> 8) & 0xff,
            .archRequirements = metadata.value(QStringLiteral("archlevel")).toInt() | debug};
}

inline auto bundleManifest(const QString &id, const QString &name, const QString &library,
                           const BundleCompatibility &compatibility) -> QByteArray {
    QJsonObject declared{{QStringLiteral("iid"), compatibility.iid},
                         {QStringLiteral("target_os"), QLatin1String(BUNDLE_TARGET_OS)},
                         {QStringLiteral("target_arch"), QLatin1String(BUNDLE_TARGET_ARCH)},
                         {QStringLiteral("qt_major"), compatibility.qtMajor},
                         {QStringLiteral("qt_minor"), compatibility.qtMinor},
                         {QStringLiteral("arch_requirements"), compatibility.archRequirements}};
    QJsonObject entry{{QStringLiteral("kind"), QStringLiteral("native")},
                      {QStringLiteral("library"), library},
                      {QStringLiteral("compatibility"), declared}};
    QJsonObject manifest{{QStringLiteral("format_version"), 1},
                         {QStringLiteral("id"), id},
                         {QStringLiteral("name"), name},
                         {QStringLiteral("version"), QStringLiteral("1.0.0")},
                         {QStringLiteral("description"), name},
                         {QStringLiteral("author"), QStringLiteral("Silent tests")},
                         {QStringLiteral("capabilities"), QJsonArray()},
                         {QStringLiteral("entry"), entry},
                         {QStringLiteral("translations"), QJsonArray()}};
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

inline auto zipCrc32(const QByteArray &bytes) -> quint32 {
    quint32 crc = 0xffffffff;
    for (auto byte : bytes) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xedb88320 : crc >> 1;
        }
    }
    return ~crc;
}

template <typename T> void appendLittleEndian(QByteArray &bytes, T value) {
    auto little = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&little), sizeof(little));
}

// A ZIP of `entries` (name, content) in order, stored uncompressed.
inline auto storedZip(const QList<QPair<QString, QByteArray>> &entries) -> QByteArray {
    // 1980-01-01 00:00, the first date a ZIP can hold.
    constexpr quint16 c_dos_date = (1 << 5) | 1;
    QByteArray archive;
    QByteArray directory;
    for (const auto &[name, content] : entries) {
        auto path = name.toUtf8();
        auto offset = archive.size();
        auto crc = zipCrc32(content);
        // Local header: version 2.0, no flags, stored, time 0.
        appendLittleEndian(archive, quint32(0x04034b50));
        appendLittleEndian(archive, quint16(20));
        appendLittleEndian(archive, quint16(0));
        appendLittleEndian(archive, quint16(0));
        appendLittleEndian(archive, quint16(0));
        appendLittleEndian(archive, c_dos_date);
        appendLittleEndian(archive, crc);
        appendLittleEndian(archive, quint32(content.size()));
        appendLittleEndian(archive, quint32(content.size()));
        appendLittleEndian(archive, quint16(path.size()));
        appendLittleEndian(archive, quint16(0));
        archive.append(path).append(content);
        // Central directory record: the same fields, no comment or attributes.
        appendLittleEndian(directory, quint32(0x02014b50));
        appendLittleEndian(directory, quint16(20));
        appendLittleEndian(directory, quint16(20));
        appendLittleEndian(directory, quint16(0));
        appendLittleEndian(directory, quint16(0));
        appendLittleEndian(directory, quint16(0));
        appendLittleEndian(directory, c_dos_date);
        appendLittleEndian(directory, crc);
        appendLittleEndian(directory, quint32(content.size()));
        appendLittleEndian(directory, quint32(content.size()));
        appendLittleEndian(directory, quint16(path.size()));
        appendLittleEndian(directory, quint16(0));
        appendLittleEndian(directory, quint16(0));
        appendLittleEndian(directory, quint16(0));
        appendLittleEndian(directory, quint16(0));
        appendLittleEndian(directory, quint32(0));
        appendLittleEndian(directory, quint32(offset));
        directory.append(path);
    }
    auto directoryOffset = archive.size();
    archive.append(directory);
    appendLittleEndian(archive, quint32(0x06054b50));
    appendLittleEndian(archive, quint16(0));
    appendLittleEndian(archive, quint16(0));
    appendLittleEndian(archive, quint16(entries.size()));
    appendLittleEndian(archive, quint16(entries.size()));
    appendLittleEndian(archive, quint32(directory.size()));
    appendLittleEndian(archive, quint32(directoryOffset));
    appendLittleEndian(archive, quint16(0));
    return archive;
}

// Imports `library` as the plugin `id`, its manifest declaring `compatibility`.
inline auto installBundle(const QString &id, const QString &name, const QByteArray &library,
                          const BundleCompatibility &compatibility) -> PluginInstallResult {
    auto payload = QStringLiteral("payload/lib%1.so").arg(id);
    auto archive = storedZip(
        {{QStringLiteral("manifest.json"), bundleManifest(id, name, payload, compatibility)},
         {payload, library}});
    QTemporaryDir directory;
    QFile bundle(directory.filePath(QStringLiteral("bundle.zip")));
    if (!directory.isValid() || !bundle.open(QIODevice::WriteOnly) ||
        bundle.write(archive) != archive.size()) {
        qFatal("plugin bundle fixture: the bundle write failed");
    }
    bundle.close();
    return installPluginBundle(bundle.fileName());
}

// Imports `library` as the plugin `id`, its manifest declaring what the library does.
inline auto installBundle(const QString &id, const QString &name, const QByteArray &library)
    -> PluginInstallResult {
    return installBundle(id, name, library, libraryCompatibility(library));
}

} // namespace smoke
