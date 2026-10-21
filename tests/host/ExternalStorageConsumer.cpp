#include "ExternalStorageConsumer.h"

#include <QApplication>
#include <QCborMap>
#include <QCborValue>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QThread>
#include <algorithm>
#include <interfaces/host.h>
#include <utility>

namespace smoke {
namespace {

constexpr int PAGE_LIMIT = 128;
constexpr int ENTRY_LIMIT = 4096;
constexpr int LISTED = PAGE_LIMIT + 2;
constexpr int TIMEOUT_MS = 60000;

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("external storage consumer: %s", message);
    }
}

void requireError(const QJsonObject &answer, const char *code, const char *message) {
    require(ExternalStorageConsumer::error(answer) == QLatin1String(code), message);
}

void requireDone(const QJsonObject &answer, const char *message) {
    require(ExternalStorageConsumer::error(answer).isEmpty(), message);
}

// The hop a process boundary adds: the message leaves as CBOR and comes back unchanged.
auto transport(const QJsonObject &message) -> QJsonObject {
    QCborParserError parsed;
    auto value =
        QCborValue::fromCbor(QCborMap::fromJsonObject(message).toCborValue().toCbor(), &parsed);
    require(parsed.error == QCborError::NoError && value.isMap(),
            "the message crosses a CBOR transport");
    auto copy = value.toMap().toJsonObject();
    require(copy == message, "the CBOR transport keeps the message unchanged");
    return copy;
}

// An answer names logical paths only: no host location, nothing outside the storage.
// NOLINTNEXTLINE(misc-no-recursion)
void requireOpaque(const QJsonValue &value, const QString &key) {
    if (value.isObject()) {
        auto object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            requireOpaque(it.value(), it.key());
        }
        return;
    }
    if (value.isArray()) {
        for (const auto &item : value.toArray()) {
            requireOpaque(item, key);
        }
        return;
    }
    auto text = value.toString();
    require(!text.contains(QStringLiteral(".silent")), "an answer carries no host path");
    if (key == QStringLiteral("path")) {
        require(!text.startsWith(QLatin1Char('/')) &&
                    !text.split(QLatin1Char('/')).contains(QStringLiteral("..")),
                "an answered path is logical and relative");
    }
}

auto binary() -> QByteArray {
    QByteArray bytes;
    for (int i = 0; i < 256; ++i) {
        bytes.append(static_cast<char>(i));
    }
    return bytes;
}

auto entry(const QJsonObject &answer) -> QJsonObject {
    return answer.value(QStringLiteral("entry")).toObject();
}
} // namespace

ExternalStorageConsumer::ExternalStorageConsumer(Host *host, IModule *caller, QString label)
    : m_storage(host->storage(caller)),
      m_label(std::move(label)) {
    if (m_storage != nullptr) {
        connect(m_storage, &IStorage::completed, this, &ExternalStorageConsumer::onCompleted,
                Qt::UniqueConnection);
    }
}

auto ExternalStorageConsumer::storage() const -> IStorage * {
    return m_storage;
}

auto ExternalStorageConsumer::send(QJsonObject request) -> QString {
    auto id = QStringLiteral("%1-%2").arg(m_label).arg(++m_next);
    request.insert(QStringLiteral("id"), id);
    m_sent.insert(id);
    sendRaw(QJsonDocument(transport(request)).toJson(QJsonDocument::Compact));
    return id;
}

void ExternalStorageConsumer::sendRaw(const QByteArray &message) {
    require(m_storage != nullptr, "the storage handle is live");
    m_storage->request(message);
}

void ExternalStorageConsumer::cancel(const QString &id) {
    require(m_storage != nullptr, "the storage handle is live");
    m_storage->cancel(id);
}

auto ExternalStorageConsumer::wait(const QString &id) -> QJsonObject {
    QElapsedTimer deadline;
    deadline.start();
    while (!m_answers.contains(id) && deadline.elapsed() < TIMEOUT_MS) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(m_answers.contains(id), "a storage request is answered");
    return m_answers.value(id);
}

auto ExternalStorageConsumer::call(const QJsonObject &request) -> QJsonObject {
    return wait(send(request));
}

auto ExternalStorageConsumer::waitStray() -> QJsonObject {
    QElapsedTimer deadline;
    deadline.start();
    while (m_strays.isEmpty() && deadline.elapsed() < TIMEOUT_MS) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(!m_strays.isEmpty(), "a message without a usable id is still answered");
    return m_strays.takeFirst();
}

auto ExternalStorageConsumer::answered(const QString &id) const -> bool {
    return m_answers.contains(id);
}

auto ExternalStorageConsumer::open() -> QString {
    auto discovered = call({{QStringLiteral("op"), QStringLiteral("discover")}});
    requireDone(discovered, "the caller discovers its storage");
    auto storages = discovered.value(QStringLiteral("storages")).toArray();
    require(storages.size() == 1, "only the caller's private storage is offered");
    auto id = storages.first().toObject().value(QStringLiteral("id")).toString();
    require(!id.isEmpty(), "a discovered storage has an id");
    requireError(call({{QStringLiteral("op"), QStringLiteral("open")},
                       {QStringLiteral("storage"), id + QStringLiteral("-other")}}),
                 "not_found", "only a discovered storage opens");
    auto opened =
        call({{QStringLiteral("op"), QStringLiteral("open")}, {QStringLiteral("storage"), id}});
    requireDone(opened, "the caller opens its private storage");
    auto handle = opened.value(QStringLiteral("storage")).toString();
    require(!handle.isEmpty(), "opening answers an opaque handle");
    return handle;
}

auto ExternalStorageConsumer::write(const QString &storage, const QString &path,
                                    const QByteArray &data, const QString &mode) -> QJsonObject {
    return call({{QStringLiteral("op"), QStringLiteral("write")},
                 {QStringLiteral("storage"), storage},
                 {QStringLiteral("path"), path},
                 {QStringLiteral("data"), QString::fromLatin1(data.toBase64())},
                 {QStringLiteral("mode"), mode}});
}

auto ExternalStorageConsumer::read(const QString &storage, const QString &path) -> QJsonObject {
    return call({{QStringLiteral("op"), QStringLiteral("read")},
                 {QStringLiteral("storage"), storage},
                 {QStringLiteral("path"), path}});
}

auto ExternalStorageConsumer::inspect(const QString &storage, const QString &path) -> QJsonObject {
    return call({{QStringLiteral("op"), QStringLiteral("inspect")},
                 {QStringLiteral("storage"), storage},
                 {QStringLiteral("path"), path}});
}

auto ExternalStorageConsumer::mkdir(const QString &storage, const QString &path) -> QJsonObject {
    return call({{QStringLiteral("op"), QStringLiteral("mkdir")},
                 {QStringLiteral("storage"), storage},
                 {QStringLiteral("path"), path}});
}

auto ExternalStorageConsumer::rename(const QString &storage, const QString &path, const QString &to)
    -> QJsonObject {
    return call({{QStringLiteral("op"), QStringLiteral("rename")},
                 {QStringLiteral("storage"), storage},
                 {QStringLiteral("path"), path},
                 {QStringLiteral("to"), to}});
}

auto ExternalStorageConsumer::remove(const QString &storage, const QString &path, bool recursive)
    -> QJsonObject {
    return call({{QStringLiteral("op"), QStringLiteral("remove")},
                 {QStringLiteral("storage"), storage},
                 {QStringLiteral("path"), path},
                 {QStringLiteral("recursive"), recursive}});
}

auto ExternalStorageConsumer::migrate(const QString &storage, const QString &path) -> QJsonObject {
    return call({{QStringLiteral("op"), QStringLiteral("migrate")},
                 {QStringLiteral("storage"), storage},
                 {QStringLiteral("path"), path}});
}

auto ExternalStorageConsumer::listAll(const QString &storage, const QString &path, int limit)
    -> QStringList {
    QStringList names;
    QString after;
    while (true) {
        QJsonObject request{{QStringLiteral("op"), QStringLiteral("list")},
                            {QStringLiteral("storage"), storage},
                            {QStringLiteral("path"), path},
                            {QStringLiteral("limit"), limit}};
        if (!after.isEmpty()) {
            request.insert(QStringLiteral("after"), after);
        }
        auto page = call(request);
        requireDone(page, "a listing page is answered");
        auto entries = page.value(QStringLiteral("entries")).toArray();
        require(entries.size() <= limit, "a listing page stays within its limit");
        for (const auto &value : entries) {
            auto listed = value.toObject();
            auto kind = listed.value(QStringLiteral("kind")).toString();
            require(kind == QStringLiteral("file") || kind == QStringLiteral("directory"),
                    "a listed entry is a file or a directory");
            names.append(
                listed.value(QStringLiteral("path")).toString().section(QLatin1Char('/'), -1));
        }
        require(names.size() <= ENTRY_LIMIT, "pagination terminates");
        after = page.value(QStringLiteral("next")).toString();
        if (after.isEmpty()) {
            return names;
        }
        require(!entries.isEmpty(), "a page with a next cursor lists entries");
    }
}

void ExternalStorageConsumer::runContract(const QString &root) {
    auto storage = open();
    contractFiles(storage, root);
    contractRename(storage, root);
    contractRemove(storage, root);
    contractListing(storage, root);
    contractRefusals(storage, root);
}

auto ExternalStorageConsumer::data(const QJsonObject &answer) -> QByteArray {
    auto decoded =
        QByteArray::fromBase64Encoding(answer.value(QStringLiteral("data")).toString().toLatin1(),
                                       QByteArray::AbortOnBase64DecodingErrors);
    require(decoded.decodingStatus == QByteArray::Base64DecodingStatus::Ok,
            "read data is valid base64");
    return decoded.decoded;
}

auto ExternalStorageConsumer::error(const QJsonObject &answer) -> QString {
    return answer.value(QStringLiteral("error")).toString();
}

void ExternalStorageConsumer::onCompleted(const QByteArray &message) {
    require(message.size() <= IStorage::MAX_MESSAGE_SIZE,
            "an answer stays within the message bound");
    QJsonParseError parsed;
    auto document = QJsonDocument::fromJson(message, &parsed);
    require(parsed.error == QJsonParseError::NoError && document.isObject(),
            "an answer is one JSON object");
    auto answer = transport(document.object());
    require(answer.value(QStringLiteral("error")).isString(), "every answer carries its error");
    requireOpaque(answer, {});
    auto id = answer.value(QStringLiteral("id")).toString();
    if (!m_sent.contains(id)) {
        m_strays.append(answer);
        return;
    }
    require(!m_answers.contains(id), "a request is answered once");
    m_answers.insert(id, answer);
}

void ExternalStorageConsumer::contractFiles(const QString &storage, const QString &root) {
    auto one = root + QStringLiteral("/a/one.txt");
    auto two = root + QStringLiteral("/a/b/two.bin");
    auto three = root + QStringLiteral("/a/three.txt");
    auto none = root + QStringLiteral("/a/none.txt");
    requireDone(mkdir(storage, root), "mkdir creates the contract root");
    requireError(mkdir(storage, root), "already_exists", "mkdir refuses an existing entry");
    requireDone(mkdir(storage, root + QStringLiteral("/a")), "mkdir creates a/");
    requireDone(mkdir(storage, root + QStringLiteral("/a/b")), "mkdir creates a nested directory");
    requireError(mkdir(storage, root + QStringLiteral("/x/y")), "not_found",
                 "mkdir needs the parent directory");
    requireDone(write(storage, one, "one", QStringLiteral("create")), "create writes a new file");
    requireDone(write(storage, two, binary(), QStringLiteral("create")),
                "create writes a nested binary file");
    require(data(read(storage, one)) == "one" && data(read(storage, two)) == binary(),
            "independent files keep their own bytes");

    requireError(write(storage, one, "other", QStringLiteral("create")), "already_exists",
                 "create refuses an existing file");
    require(data(read(storage, one)) == "one", "a refused create leaves the file");
    requireError(write(storage, none, "x", QStringLiteral("replace")), "not_found",
                 "replace refuses a missing file");
    requireError(read(storage, none), "not_found", "reading a missing file is not_found");
    requireError(inspect(storage, none), "not_found", "inspecting a missing file is not_found");

    requireDone(write(storage, one, "uno", QStringLiteral("replace")),
                "replace rewrites an existing file");
    require(data(read(storage, one)) == "uno" && data(read(storage, two)) == binary(),
            "replacing one file leaves the other");
    requireDone(write(storage, three, "3", QStringLiteral("upsert")),
                "upsert creates a missing file");
    requireDone(write(storage, three, "three", QStringLiteral("upsert")),
                "upsert replaces an existing file");
    require(data(read(storage, three)) == "three", "upsert keeps the last bytes");
    requireDone(write(storage, root + QStringLiteral("/a/empty"), {}, QStringLiteral("create")),
                "an empty file is a file");
    require(entry(inspect(storage, root + QStringLiteral("/a/empty")))
                    .value(QStringLiteral("size"))
                    .toInteger() == 0,
            "an empty file has size 0");

    auto file = entry(inspect(storage, one));
    require(file.value(QStringLiteral("path")).toString() == one &&
                file.value(QStringLiteral("kind")).toString() == QStringLiteral("file") &&
                file.value(QStringLiteral("size")).toInteger() == 3,
            "inspect describes a file");
    auto directory = entry(inspect(storage, root + QStringLiteral("/a/b")));
    require(directory.value(QStringLiteral("kind")).toString() == QStringLiteral("directory"),
            "inspect describes a directory");

    requireError(
        write(storage, root + QStringLiteral("/missing/x.txt"), "x", QStringLiteral("upsert")),
        "not_found", "a write needs its parent directory");
    requireError(inspect(storage, root + QStringLiteral("/missing")), "not_found",
                 "a refused write creates no parent");
    requireError(write(storage, one + QStringLiteral("/below.txt"), "x", QStringLiteral("upsert")),
                 "not_directory", "a file holds no entry");
    requireError(write(storage, root + QStringLiteral("/a/b"), "x", QStringLiteral("upsert")),
                 "is_directory", "a directory is never replaced by a file");
    requireError(read(storage, root + QStringLiteral("/a/b")), "is_directory",
                 "a directory is not read as a file");
}

void ExternalStorageConsumer::contractRename(const QString &storage, const QString &root) {
    auto one = root + QStringLiteral("/a/one.txt");
    auto three = root + QStringLiteral("/a/three.txt");
    auto moved = root + QStringLiteral("/a/b/moved.txt");
    requireError(rename(storage, one, three), "already_exists",
                 "rename refuses an existing destination");
    require(data(read(storage, one)) == "uno" && data(read(storage, three)) == "three",
            "a refused rename replaces nothing");
    requireDone(rename(storage, one, moved), "rename moves a file to another directory");
    requireError(read(storage, one), "not_found", "a moved file leaves its old path");
    require(data(read(storage, moved)) == "uno", "a moved file keeps its bytes");
    requireError(rename(storage, one, root + QStringLiteral("/a/again.txt")), "not_found",
                 "renaming a missing entry is not_found");
    requireError(rename(storage, root + QStringLiteral("/a"), root + QStringLiteral("/a/b/a")),
                 "invalid_path", "a directory never moves below itself");
    requireDone(rename(storage, root + QStringLiteral("/a/b"), root + QStringLiteral("/c")),
                "rename moves a whole directory");
    require(data(read(storage, root + QStringLiteral("/c/two.bin"))) == binary() &&
                data(read(storage, root + QStringLiteral("/c/moved.txt"))) == "uno",
            "a moved directory keeps its files");
    requireError(inspect(storage, root + QStringLiteral("/a/b")), "not_found",
                 "a moved directory leaves its old path");
}

void ExternalStorageConsumer::contractRemove(const QString &storage, const QString &root) {
    auto directory = root + QStringLiteral("/c");
    requireError(remove(storage, directory, false), "not_empty",
                 "removing a non-empty directory needs recursive");
    require(data(read(storage, directory + QStringLiteral("/two.bin"))) == binary(),
            "a refused removal leaves the tree");
    requireDone(remove(storage, directory, true),
                "recursive removal removes a non-empty directory");
    requireError(inspect(storage, directory), "not_found",
                 "recursive removal removes the directory");
    requireError(read(storage, directory + QStringLiteral("/moved.txt")), "not_found",
                 "recursive removal removes every entry below");
    requireError(remove(storage, directory, true), "not_found",
                 "removing a missing entry is not_found");
    requireDone(remove(storage, root + QStringLiteral("/a/three.txt"), false),
                "remove removes a file");
    requireError(read(storage, root + QStringLiteral("/a/three.txt")), "not_found",
                 "a removed file is gone");
    requireDone(mkdir(storage, root + QStringLiteral("/empty")),
                "mkdir creates an empty directory");
    requireDone(remove(storage, root + QStringLiteral("/empty"), false),
                "an empty directory is removed without recursive");
}

void ExternalStorageConsumer::contractListing(const QString &storage, const QString &root) {
    auto directory = root + QStringLiteral("/page");
    requireDone(mkdir(storage, directory), "mkdir creates the listing directory");
    QStringList expected;
    for (int i = 0; i < LISTED; ++i) {
        auto name = QStringLiteral("f%1").arg(i, 3, 10, QLatin1Char('0'));
        expected.append(name);
        requireDone(write(storage, directory + QLatin1Char('/') + name, QByteArray::number(i),
                          QStringLiteral("create")),
                    "create writes a listed file");
    }
    requireDone(mkdir(storage, directory + QStringLiteral("/sub")),
                "mkdir creates a listed directory");
    expected.append(QStringLiteral("sub"));
    std::ranges::sort(expected);

    auto first = call({{QStringLiteral("op"), QStringLiteral("list")},
                       {QStringLiteral("storage"), storage},
                       {QStringLiteral("path"), directory}});
    requireDone(first, "a listing is answered");
    require(first.value(QStringLiteral("entries")).toArray().size() == PAGE_LIMIT &&
                !first.value(QStringLiteral("next")).toString().isEmpty(),
            "a listing longer than a page is paginated by default");
    for (auto limit : {0, PAGE_LIMIT + 1}) {
        requireError(call({{QStringLiteral("op"), QStringLiteral("list")},
                           {QStringLiteral("storage"), storage},
                           {QStringLiteral("path"), directory},
                           {QStringLiteral("limit"), limit}}),
                     "invalid_request", "a page limit outside 1 to 128 is refused");
    }
    requireError(call({{QStringLiteral("op"), QStringLiteral("list")},
                       {QStringLiteral("storage"), storage},
                       {QStringLiteral("path"), directory + QStringLiteral("/f000")}}),
                 "not_directory", "a file is not listed");
    for (auto limit : {PAGE_LIMIT, 7, 1}) {
        auto names = listAll(storage, directory, limit);
        std::ranges::sort(names);
        require(names == expected, "pagination lists every entry exactly once");
    }
}

void ExternalStorageConsumer::contractRefusals(const QString &storage, const QString &root) {
    auto before = listAll(storage, root, PAGE_LIMIT);
    for (const auto &path : {QString(), QStringLiteral("/absolute"), QStringLiteral(".."),
                             QStringLiteral("../escape"), root + QStringLiteral("/../escape"),
                             root + QStringLiteral("//double"), root + QStringLiteral("/./dot"),
                             root + QStringLiteral("/a/.."), root + QStringLiteral("/trailing/")}) {
        requireError(write(storage, path, "x", QStringLiteral("upsert")), "invalid_path",
                     "an invalid logical path is refused");
    }
    require(listAll(storage, root, PAGE_LIMIT) == before,
            "refused paths create nothing in the storage");

    QByteArray largest(IStorage::MAX_FILE_SIZE, 'L');
    requireDone(
        write(storage, root + QStringLiteral("/largest.bin"), largest, QStringLiteral("create")),
        "a file of exactly the file bound is accepted");
    require(data(read(storage, root + QStringLiteral("/largest.bin"))) == largest,
            "the largest file reads back whole");
    requireError(write(storage, root + QStringLiteral("/over.bin"),
                       QByteArray(IStorage::MAX_FILE_SIZE + 1, 'O'), QStringLiteral("create")),
                 "too_large", "a file over the bound is refused");
    requireError(inspect(storage, root + QStringLiteral("/over.bin")), "not_found",
                 "a refused oversized file is not published");
    requireError(write(storage, root + QStringLiteral("/mode.txt"), "x", QStringLiteral("append")),
                 "invalid_request", "an unknown write mode is refused");
    requireError(call({{QStringLiteral("op"), QStringLiteral("write")},
                       {QStringLiteral("storage"), storage},
                       {QStringLiteral("path"), root + QStringLiteral("/mode.txt")},
                       {QStringLiteral("data"), QStringLiteral("not base64!")},
                       {QStringLiteral("mode"), QStringLiteral("create")}}),
                 "invalid_request", "data that is not base64 is refused");
    requireError(inspect(storage, root + QStringLiteral("/mode.txt")), "not_found",
                 "refused writes write nothing");
    requireError(call({{QStringLiteral("op"), QStringLiteral("truncate")},
                       {QStringLiteral("storage"), storage},
                       {QStringLiteral("path"), root}}),
                 "invalid_request", "an unknown operation is refused");
    requireError(call({{QStringLiteral("op"), QStringLiteral("read")},
                       {QStringLiteral("path"), root + QStringLiteral("/largest.bin")}}),
                 "invalid_request", "an operation without a handle is refused");
    requireError(read(QStringLiteral("forged-handle"), root + QStringLiteral("/largest.bin")),
                 "invalid_handle", "a forged handle is refused");

    sendRaw("not json");
    auto malformed = waitStray();
    require(error(malformed) == QStringLiteral("invalid_request") &&
                malformed.value(QStringLiteral("id")).toString().isEmpty(),
            "a malformed message is refused with an empty id");
    sendRaw("[]");
    requireError(waitStray(), "invalid_request", "a message that is not an object is refused");
    sendRaw(R"({"op":"discover"})");
    requireError(waitStray(), "invalid_request", "a message without an id is refused");
    QByteArray oversized(R"({"id":"oversized","op":"discover","pad":")");
    oversized.append(QByteArray(IStorage::MAX_MESSAGE_SIZE, 'p'));
    oversized.append(R"("})");
    sendRaw(oversized);
    auto refused = waitStray();
    require(error(refused) == QStringLiteral("too_large") &&
                refused.value(QStringLiteral("id")).toString().isEmpty(),
            "a message over the message bound is refused with an empty id");
}

} // namespace smoke
