#include "host/PluginLibrary.h"
#include "host/PluginMetadata.h"
#include "host/PluginRegistry.h"
#include <QCborMap>
#include <QCborValue>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtPlugin>
#include <cstddef>
#include <silent.h>
#include <string>

namespace smoke {

namespace {
auto metadataBytes(const QCborMap &map) -> QByteArray {
    QPluginMetaData::MagicHeader header;
    return QByteArray(reinterpret_cast<const char *>(&header), sizeof(header)) +
           QCborValue(map).toCbor();
}

void metadataRejectionSmoke(const QByteArray &bytes) {
    auto expected = QStringLiteral("dev.silent.IPlugin/4.0");
    if (pluginMetadataError(bytes, QStringLiteral("dev.silent.IPlugin/3.0")).isEmpty()) {
        qFatal("ABI 3 expectation must reject an ABI 4 library");
    }
    QCborMap nested;
    nested.insert(QStringLiteral("IID"), expected);
    QCborMap metadata;
    metadata.insert(4, nested);
    if (pluginMetadataError(metadataBytes(metadata), expected).isEmpty()) {
        qFatal("nested metadata IID must not satisfy the root IID check");
    }
    metadata.insert(2, expected);
    auto valid = metadataBytes(metadata);
    if (!pluginMetadataError(valid, expected).isEmpty()) {
        qFatal("regular Qt metadata with the exact root IID must be accepted");
    }
    if (pluginMetadataError(valid + valid, expected).isEmpty()) {
        qFatal("duplicate Qt metadata markers must be rejected");
    }
    QPluginMetaData::MagicHeader header;
    auto prefix = QByteArray(reinterpret_cast<const char *>(&header), sizeof(header));
    auto iidEntry = QByteArray::fromHex("02") + QCborValue(expected).toCbor();
    auto duplicateIid = prefix + QByteArray::fromHex("a2") + iidEntry + iidEntry;
    if (pluginMetadataError(duplicateIid, expected).isEmpty()) {
        qFatal("duplicate root IID keys must be rejected");
    }
    if (pluginMetadataError(prefix, expected).isEmpty() ||
        pluginMetadataError(valid.first(valid.size() - 1), expected).isEmpty()) {
        qFatal("truncated metadata header or CBOR must be rejected");
    }
    metadata.insert(2, expected + QStringLiteral("extra"));
    if (pluginMetadataError(metadataBytes(metadata), expected).isEmpty()) {
        qFatal("root IID must match exactly, not just its ABI prefix");
    }
    metadata.insert(2, 4);
    if (pluginMetadataError(metadataBytes(metadata), expected).isEmpty()) {
        qFatal("non-string root IID must be rejected");
    }

    auto note = QByteArray(QPluginMetaData::ElfNoteHeader::NoteName,
                           sizeof(QPluginMetaData::ElfNoteHeader::NoteName));
    auto noteOffset = bytes.indexOf(note);
    if (noteOffset < 0) {
        qFatal("Linux ABI fixture must contain a Qt metadata note");
    }
    auto headerOffset = noteOffset +
                        static_cast<qsizetype>(offsetof(QPluginMetaData::ElfNoteHeader, header)) -
                        static_cast<qsizetype>(offsetof(QPluginMetaData::ElfNoteHeader, name));
    auto missing = bytes;
    missing.replace(noteOffset, note.size(), QByteArray(note.size(), '\0'));
    auto malformed = bytes;
    malformed[headerOffset] = char(0xff);
    auto invalidCbor = bytes;
    invalidCbor[headerOffset + sizeof(QPluginMetaData::Header)] = char(0xff);
    for (const auto &invalid : {missing, malformed, invalidCbor}) {
        PluginLibrary library;
        if (library.load(invalid, QStringLiteral("invalid-metadata.so")) ||
            library.instance() != nullptr || library.errorString().isEmpty()) {
            qFatal("missing or malformed metadata must fail before loading the library");
        }
    }
}
} // namespace

void pluginAbiSmoke() {
    for (auto version : {2, 3, 4}) {
        auto legacy = version != 4;
        QTemporaryDir directory;
        if (!directory.isValid()) {
            qFatal("ABI fixture directory creation failed");
        }
        auto sentinelPath = directory.filePath(QStringLiteral("execution-sentinel"));
        auto previousSentinel = qgetenv("SILENT_ABI_SENTINEL");
        auto hadSentinel = qEnvironmentVariableIsSet("SILENT_ABI_SENTINEL");
        qputenv("SILENT_ABI_SENTINEL", QFile::encodeName(sentinelPath));
        auto id = QStringLiteral("abi%1").arg(version);
        auto library = QString::fromUtf8(SILENT_ABI4_FIXTURE_PATH);
        if (version == 2) {
            library = QString::fromUtf8(SILENT_ABI2_FIXTURE_PATH);
        } else if (version == 3) {
            library = QString::fromUtf8(SILENT_ABI3_FIXTURE_PATH);
        }
        auto libraryPath = QDir(directory.path()).filePath(QStringLiteral("lib%1.so").arg(id));
        if (!QFile::copy(library, libraryPath)) {
            qFatal("ABI fixture library copy failed");
        }
        if (!legacy) {
            QFile fixture(libraryPath);
            if (!fixture.open(QIODevice::ReadOnly)) {
                qFatal("ABI 4 fixture read failed");
            }
            metadataRejectionSmoke(fixture.readAll());
            if (QFile::exists(sentinelPath)) {
                qFatal("rejected metadata must run neither initializer nor constructor");
            }
        }
        QFile metadata(QDir(directory.path()).filePath(id + QStringLiteral(".json")));
        auto json = QStringLiteral("{\"id\":\"%1\",\"name\":\"ABI fixture\"}").arg(id).toUtf8();
        if (!metadata.open(QIODevice::WriteOnly) || metadata.write(json) != json.size()) {
            qFatal("ABI fixture sidecar write failed");
        }
        metadata.close();
        auto installed = ::plugin_install(rust::String(directory.path().toStdString()));
        if (!installed.is_ok) {
            qFatal("ABI fixture install failed: %s", installed.error.c_str());
        }
        {
            PluginRegistry registry;
            registry.rescanDiscoveredPlugins();
            auto enabled = registry.setEnabled(id, true);
            if (enabled == legacy || registry.isPluginEnabled(id) == legacy) {
                qFatal("registry must reject ABI 2/3 and accept ABI 4 binaries");
            }
            if (legacy &&
                registry.pluginLoadError(id) !=
                    QStringLiteral(
                        "incompatible Qt plugin root IID (expected dev.silent.IPlugin/4.0)")) {
                qFatal("old ABI binary must fail at the metadata admission gate");
            }
            if (!registry.enabledModulesForLauncher().empty()) {
                qFatal("ABI fixture must register no modules or feed instances");
            }
        }
        QFile sentinel(sentinelPath);
        if (legacy) {
            if (sentinel.exists()) {
                qFatal("ABI 2/3 rejection must run neither initializer nor constructor");
            }
        } else if (!sentinel.open(QIODevice::ReadOnly) ||
                   sentinel.readAll() != QByteArray("initializer\nconstructor\n")) {
            qFatal("ABI 4 admission must run the library initializer and root constructor once");
        }
        if (hadSentinel) {
            qputenv("SILENT_ABI_SENTINEL", previousSentinel);
        } else {
            qunsetenv("SILENT_ABI_SENTINEL");
        }
        auto error = ::plugin_remove(rust::String(id.toStdString()));
        if (!error.empty()) {
            qFatal("ABI fixture removal failed: %s", error.c_str());
        }
    }
}

} // namespace smoke
