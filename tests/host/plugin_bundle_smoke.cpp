// Drives one real producer bundle (a plugin ZIP) through the host import and enable
// path: the importer the Settings drop calls, with this host's Qt information, then the
// real PluginRegistry.

#include "PluginBundleFixture.h"
#include "PluginStopProbe.h"
#include "host/Host.h"
#include "host/PluginMetadata.h"
#include "host/PluginRegistry.h"
#include "interfaces/feed.h"
#include "interfaces/instance.h"
#include "interfaces/module.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTypes>
#include <algorithm>
#include <common.h>
#include <silent.h>
#include <string>

namespace smoke {
namespace {

constexpr int TIMEOUT_MS = 10000;

// The public feed a producer exposes: fee recommendations (#52) or fiat prices (#53).
enum class FeedShape { None, Fee, Price };

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("plugin bundle smoke: %s", message);
    }
}

auto text(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

auto install(const QString &path) -> PluginInstallResult {
    auto result = installPluginBundle(path);
    QCoreApplication::processEvents();
    return result;
}

// The installed manifest of `plugin_id`, as the importer sealed it.
auto installedManifest(const QString &plugin_id) -> QJsonObject {
    for (const auto &bundle : ::plugin_bundles()) {
        auto manifest = QJsonDocument::fromJson(text(bundle).toUtf8()).object();
        if (manifest.value(QStringLiteral("id")).toString() == plugin_id) {
            return manifest;
        }
    }
    return {};
}

auto isInstalled(const QString &plugin_id) -> bool {
    auto installed = ::plugin_metadata();
    return std::ranges::any_of(
        installed, [&plugin_id](const auto &bundle) { return text(bundle.id) == plugin_id; });
}

auto strings(const QJsonValue &value) -> QStringList {
    QStringList items;
    for (const auto &item : value.toArray()) {
        items.append(item.toString());
    }
    return items;
}

// The producers whose public feed this smoke knows, by plugin id.
auto feedShape(const QString &plugin_id) -> FeedShape {
    if (plugin_id == QStringLiteral("feerate")) {
        return FeedShape::Fee;
    }
    if (plugin_id == QStringLiteral("fiat")) {
        return FeedShape::Price;
    }
    return FeedShape::None;
}

// The metadata Settings lists for the plugin.
auto listedMeta(PluginRegistry &registry, const QString &plugin_id) -> QMap<QString, QString> {
    for (auto *plugin : registry.plugins()) {
        auto meta = plugin->meta();
        if (meta.value(QStringLiteral("id")) == plugin_id) {
            return meta;
        }
    }
    return {};
}

auto loadedPlugin(PluginRegistry &registry, const QString &plugin_id) -> IPlugin * {
    for (auto *plugin : registry.plugins()) {
        auto *object = dynamic_cast<QObject *>(plugin);
        if (object != nullptr && object->qt_metacast(SILENT_IPlugin_IID) != nullptr &&
            plugin->meta().value(QStringLiteral("id")) == plugin_id) {
            return plugin;
        }
    }
    return nullptr;
}

void requireMetaField(const QMap<QString, QString> &meta, const QJsonObject &manifest,
                      const QString &field, const char *what) {
    if (meta.value(field) != manifest.value(field).toString()) {
        qFatal("plugin bundle smoke: %s '%s' is '%s', the manifest says '%s'", what,
               qPrintable(field), qPrintable(meta.value(field)),
               qPrintable(manifest.value(field).toString()));
    }
}

// Listed from the manifest, its code not loaded.
void requireListedMetadata(PluginRegistry &registry, const QJsonObject &manifest) {
    auto id = manifest.value(QStringLiteral("id")).toString();
    require(!registry.isPluginEnabled(id), "the plugin is listed disabled");
    require(loadedPlugin(registry, id) == nullptr, "a listed plugin runs no code until enabled");
    auto meta = listedMeta(registry, id);
    QStringList fields{QStringLiteral("name"), QStringLiteral("version"),
                       QStringLiteral("description"), QStringLiteral("author")};
    for (const auto &field : fields) {
        requireMetaField(meta, manifest, field, "listed metadata");
    }
    require(meta.value(QStringLiteral("capabilities")) ==
                strings(manifest.value(QStringLiteral("capabilities"))).join(QLatin1Char(',')),
            "the listed metadata has the manifest capabilities");
    require(registry.pluginName(id) == manifest.value(QStringLiteral("name")).toString() &&
                registry.pluginVersion(id) ==
                    manifest.value(QStringLiteral("version")).toString() &&
                registry.pluginDescription(id) ==
                    manifest.value(QStringLiteral("description")).toString(),
            "the registry names the plugin from its manifest");
}

// The live instance of `plugin_id`'s modules that serves a feed, waiting for its
// auto start.
auto feedInstance(PluginRegistry &registry, const QString &plugin_id) -> IInstance * {
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "the host lists its live instances");
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < TIMEOUT_MS) {
        for (auto *instance : host->liveInstances()) {
            if (instance != nullptr && instance->feed() != nullptr &&
                registry.instanceOwnerPluginId(instance->id()) == plugin_id) {
                return instance;
            }
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return nullptr;
}

// Only the shape: a request the feed serves gets a request id, one it does not gets
// 0. The answers come from the network and are not waited for.
void requireFeedShape(PluginRegistry &registry, const QString &plugin_id) {
    auto shape = feedShape(plugin_id);
    if (shape == FeedShape::None) {
        return;
    }
    auto *instance = feedInstance(registry, plugin_id);
    require(instance != nullptr, "the producer starts its feed instance");
    auto *feed = instance->feed();
    require(feed->implemented(), "the producer feed is implemented");
    auto fees = feed->feeCapabilities() != 0;
    auto prices = feed->currencies() != 0;
    switch (shape) {
    case FeedShape::Fee:
        require(fees && !prices, "the fee producer serves fee recommendations, not prices");
        break;
    case FeedShape::Price:
        require(prices && !fees, "the fiat producer serves prices, not fee recommendations");
        break;
    case FeedShape::None:
        break;
    }
}

// Loaded from the sealed bytes: its hash, root object, modules and catalogs are the
// bundle's.
void requireEnabled(PluginRegistry &registry, const QJsonObject &manifest) {
    auto id = manifest.value(QStringLiteral("id")).toString();
    if (!registry.setEnabled(id, true)) {
        qFatal("plugin bundle smoke: enable failed: %s", qPrintable(registry.pluginLoadError(id)));
    }
    QCoreApplication::processEvents();
    require(registry.isPluginEnabled(id) && registry.pluginLoadError(id).isEmpty(),
            "the imported plugin enables");
    auto opened = ::plugin_library(rust::String(id.toStdString()));
    require(opened.is_ok, "the installed library opens");
    QByteArray library(reinterpret_cast<const char *>(opened.library.data()),
                       static_cast<qsizetype>(opened.library.size()));
    require(registry.pluginHash(id) ==
                QString::fromLatin1(
                    QCryptographicHash::hash(library, QCryptographicHash::Sha256).toHex()),
            "the loaded library is the sealed payload");
    require(QStringLiteral("payload/") + text(opened.library_name) ==
                manifest.value(QStringLiteral("entry"))
                    .toObject()
                    .value(QStringLiteral("library"))
                    .toString(),
            "the loaded library is the manifest entry");
    auto *plugin = loadedPlugin(registry, id);
    require(plugin != nullptr, "the loaded root object is a current ABI Silent plugin");
    auto meta = plugin->meta();
    requireMetaField(meta, manifest, QStringLiteral("name"), "loaded root");
    requireMetaField(meta, manifest, QStringLiteral("version"), "loaded root");
    require(!plugin->modules().isEmpty(), "the loaded plugin exposes its modules");
    QStringList catalogs;
    for (const auto &catalog : opened.catalogs) {
        catalogs.append(QStringLiteral("i18n/") + text(catalog.name));
    }
    catalogs.sort();
    auto declared = strings(manifest.value(QStringLiteral("translations")));
    declared.sort();
    require(catalogs == declared, "the plugin's catalogs are the manifest translations");
    requireFeedShape(registry, id);
}

void requireDisabled(PluginRegistry &registry, const QString &plugin_id) {
    auto error = stopPlugin(&registry, plugin_id);
    if (!error.isEmpty()) {
        qFatal("plugin disable failed: %s", qPrintable(error));
    }
    require(!registry.isPluginEnabled(plugin_id) && loadedPlugin(registry, plugin_id) == nullptr,
            "a disabled plugin is unloaded");
}

auto writeFile(const QString &path, const QByteArray &bytes) -> QString {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "a bundle file is written");
    return path;
}

// A copy of the bundle at `path` under another name, or only its first half.
auto copyBundle(const QString &bundle, const QString &path, bool truncated) -> QString {
    QFile source(bundle);
    require(source.open(QIODevice::ReadOnly), "the bundle opens");
    auto bytes = source.readAll();
    if (truncated) {
        bytes.truncate(bytes.size() / 2);
    }
    return writeFile(path, bytes);
}

// Other admitted bytes under the same manifest and catalogs.
auto retaggedBundle(const QJsonObject &manifest, const QString &path) -> QString {
    auto id = manifest.value(QStringLiteral("id")).toString();
    auto opened = ::plugin_library(rust::String(id.toStdString()));
    require(opened.is_ok, "the installed library opens");
    QByteArray library(reinterpret_cast<const char *>(opened.library.data()),
                       static_cast<qsizetype>(opened.library.size()));
    library.append(QByteArray("\nplugin bundle smoke replacement"));
    auto payload =
        manifest.value(QStringLiteral("entry")).toObject().value(QStringLiteral("library"));
    QList<QPair<QString, QByteArray>> entries{
        {QStringLiteral("manifest.json"), QJsonDocument(manifest).toJson()},
        {payload.toString(), library}};
    for (const auto &catalog : opened.catalogs) {
        entries.append({QStringLiteral("i18n/") + text(catalog.name), text(catalog.text).toUtf8()});
    }
    return writeFile(path, storedZip(entries));
}

// The real library under a manifest naming another plugin: it imports, the registry
// refuses to load it as that plugin and keeps listing the manifest metadata.
void claimedIdSmoke(PluginRegistry &registry, const QJsonObject &manifest,
                    const QTemporaryDir &directory) {
    auto id = manifest.value(QStringLiteral("id")).toString();
    auto opened = ::plugin_library(rust::String(id.toStdString()));
    require(opened.is_ok, "the installed library opens");
    QByteArray library(reinterpret_cast<const char *>(opened.library.data()),
                       static_cast<qsizetype>(opened.library.size()));
    auto claimed = manifest;
    auto claimedId = id + QStringLiteral("-claimed");
    claimed.insert(QStringLiteral("id"), claimedId);
    claimed.insert(QStringLiteral("translations"), QJsonArray());
    auto path =
        writeFile(directory.filePath(QStringLiteral("claimed.zip")),
                  storedZip({{QStringLiteral("manifest.json"), QJsonDocument(claimed).toJson()},
                             {QStringLiteral("payload/") + text(opened.library_name), library}}));
    auto imported = install(path);
    if (!imported.is_ok) {
        qFatal("plugin bundle smoke: claimed-id import failed: %s", imported.error.c_str());
    }
    require(text(imported.plugin_id) == claimedId, "the bundle imports under its manifest id");
    registry.rescanDiscoveredPlugins();
    require(!registry.isPluginEnabled(claimedId), "the claimed-id plugin is listed disabled");
    require(!registry.setEnabled(claimedId, true), "a library claiming another id never enables");
    require(registry.pluginLoadError(claimedId) ==
                QStringLiteral("plugin library claims the id '%1'").arg(id),
            "the refusal names the id the library claims");
    requireListedMetadata(registry, installedManifest(claimedId));
    require(installedManifest(claimedId) == claimed, "the refused plugin keeps its manifest");
    require(stopPlugin(&registry, claimedId, true).isEmpty(), "the claimed-id plugin is removed");
}

} // namespace

void pluginBundleSmoke(const QString &bundle) {
    QTemporaryDir directory;
    require(directory.isValid(), "a private directory for the bundle copies");

    auto imported = install(bundle);
    if (!imported.is_ok) {
        qFatal("plugin bundle smoke: import failed: %s", imported.error.c_str());
    }
    auto id = text(imported.plugin_id);
    auto manifest = installedManifest(id);
    require(manifest.value(QStringLiteral("format_version")).toInt() == 1,
            "the installed manifest is the bundle's");

    PluginRegistry registry;
    registry.rescanDiscoveredPlugins();
    requireListedMetadata(registry, manifest);
    requireEnabled(registry, manifest);
    auto hash = registry.pluginHash(id);

    // Reimported under another file name while running: it stays enabled.
    auto renamed = copyBundle(bundle, directory.filePath(QStringLiteral("renamed")), false);
    auto replaced = install(renamed);
    require(replaced.is_ok && text(replaced.plugin_id) == id,
            "the bundle imports under any file name, over the enabled plugin");
    registry.rescanDiscoveredPlugins();
    require(registry.isPluginEnabled(id), "importing over an enabled plugin keeps it enabled");

    // Other library bytes stay pending until disabled and enabled.
    auto retagged =
        install(retaggedBundle(manifest, directory.filePath(QStringLiteral("retagged.zip"))));
    require(retagged.is_ok && text(retagged.plugin_id) == id,
            "other library bytes import over the enabled plugin");
    registry.rescanDiscoveredPlugins();
    require(registry.isPluginEnabled(id) && registry.pluginHash(id) == hash,
            "importing over an enabled plugin keeps its library running");
    requireDisabled(registry, id);
    requireEnabled(registry, manifest);
    require(registry.pluginHash(id) != hash, "enabled again, the plugin runs the new bytes");

    // Reimported while off: still off, listed with the same metadata.
    requireDisabled(registry, id);
    auto again = install(bundle);
    require(again.is_ok, "the bundle imports over the disabled plugin");
    registry.rescanDiscoveredPlugins();
    require(installedManifest(id) == manifest, "the replacement keeps the manifest");
    requireListedMetadata(registry, manifest);

    // A broken replacement leaves the installed bundle as it was.
    auto broken =
        install(copyBundle(bundle, directory.filePath(QStringLiteral("broken.silent")), true));
    require(!broken.is_ok && !std::string(broken.error).empty(),
            "a truncated bundle is refused with its error");
    registry.rescanDiscoveredPlugins();
    require(installedManifest(id) == manifest, "a refused bundle replaces nothing");
    requireListedMetadata(registry, manifest);
    requireEnabled(registry, manifest);
    require(registry.pluginHash(id) == hash, "the original bundle's library loads again");
    claimedIdSmoke(registry, manifest, directory);

    auto *host = Host::get();
    auto shape = feedShape(id);
    QString selectedSource;
    if (shape == FeedShape::Price) {
        auto sources = host->priceSources();
        require(!sources.isEmpty(), "the real price producer is available before removal");
        selectedSource = sources.first().id;
        host->selectPrice(selectedSource, QStringLiteral("USD"));
    } else if (shape == FeedShape::Fee) {
        auto sources = host->feeSources();
        require(!sources.isEmpty(), "the real fee producer is available before removal");
        selectedSource = sources.first().id;
        host->selectFees({.mode = FeeMode::Smart,
                          .default_source = selectedSource,
                          .smart_source = selectedSource,
                          .tier = FeeTier::Medium});
    }
    require(stopPlugin(&registry, id, true).isEmpty(), "the plugin is removed");
    require(!isInstalled(id), "a removed plugin is no longer listed");
    if (shape == FeedShape::Price) {
        auto selected = host->selectedPrice();
        require(selected.source == selectedSource && selected.currency == QStringLiteral("USD") &&
                    !selected.rate.has_value(),
                "removed price provider stays selected but unavailable, with no fallback");
    } else if (shape == FeedShape::Fee) {
        auto selected = host->selectedFees();
        require(selected.preferences.smart_source == selectedSource &&
                    selected.preferences.default_source == selectedSource &&
                    !selected.medium.has_value() && !selected.default_rate.has_value(),
                "removed fee provider stays selected but unavailable, with no fallback");
    }
}

} // namespace smoke
