#include "host/PluginLibrary.h"
#include "host/PluginMetadata.h"
#include "host/PluginRegistry.h"
#include <QCborMap>
#include <QCborValue>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QtCore/qconfig.h>
#include <QtEnvironmentVariables>
#include <QtLogging>
#include <QtPlugin>
#include <QtTypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <silent.h>
#include <utility>

namespace smoke {

namespace {
template <typename T> auto readNative(const QByteArray &bytes, qsizetype offset) -> T {
    if (offset < 0 || offset > bytes.size() || sizeof(T) > size_t(bytes.size() - offset)) {
        qFatal("native fixture structure outside file");
    }
    T value{};
    std::memcpy(&value, bytes.constData() + offset, sizeof(T));
    return value;
}

template <typename T> void writeNative(QByteArray &bytes, qsizetype offset, const T &value) {
    if (offset < 0 || offset > bytes.size() || sizeof(T) > size_t(bytes.size() - offset)) {
        qFatal("native fixture patch outside file");
    }
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

auto fixtureBytes(const char *path) -> QByteArray {
    QFile fixture(QString::fromUtf8(path));
    if (!fixture.open(QIODevice::ReadOnly)) {
        qFatal("compiled ABI fixture read failed");
    }
    return fixture.readAll();
}

auto sectionHeaderOffset(const QByteArray &bytes, const QByteArray &name) -> qsizetype {
    auto header = readNative<Elf64_Ehdr>(bytes, 0);
    auto strings =
        readNative<Elf64_Shdr>(bytes, header.e_shoff + header.e_shstrndx * header.e_shentsize);
    auto names = bytes.mid(strings.sh_offset, strings.sh_size);
    for (int index = 1; std::cmp_less(index, header.e_shnum); ++index) {
        auto offset = header.e_shoff + index * header.e_shentsize;
        auto section = readNative<Elf64_Shdr>(bytes, offset);
        if (std::cmp_less(section.sh_name, names.size()) &&
            names.mid(section.sh_name).startsWith(name + '\0')) {
            return offset;
        }
    }
    qFatal("native fixture section missing: %s", name.constData());
}

// Rewrite the compiled image in place so record regressions also pass native admission.
auto metadataContainer(const QByteArray &record) -> QByteArray {
    auto bytes = fixtureBytes(SILENT_ABI8_FIXTURE_PATH);
    auto header = readNative<Elf64_Ehdr>(bytes, 0);
    auto offset = sectionHeaderOffset(bytes, ".note.qt.metadata");
    auto section = readNative<Elf64_Shdr>(bytes, offset);
    if (std::cmp_greater(record.size(), section.sh_size)) {
        qFatal("metadata regression exceeds compiled fixture reserve");
    }
    bytes.replace(section.sh_offset, section.sh_size, QByteArray(section.sh_size, '\0'));
    bytes.replace(section.sh_offset, record.size(), record);
    auto isNote = !record.startsWith(
        QByteArray(QPluginMetaData::MagicString, sizeof(QPluginMetaData::MagicString)));
    if (!isNote) {
        auto strings =
            readNative<Elf64_Shdr>(bytes, header.e_shoff + header.e_shstrndx * header.e_shentsize);
        bytes.replace(strings.sh_offset + section.sh_name, 18,
                      QByteArray(".qtmetadata\0", 12) + QByteArray(6, '\0'));
        section.sh_type = SHT_PROGBITS;
    }
    for (int index = 0; std::cmp_less(index, header.e_phnum); ++index) {
        auto programOffset = header.e_phoff + index * header.e_phentsize;
        auto program = readNative<Elf64_Phdr>(bytes, programOffset);
        if (program.p_type == PT_NOTE && program.p_offset == section.sh_offset) {
            program.p_type = isNote ? PT_NOTE : PT_NULL;
            program.p_filesz = record.size();
            program.p_memsz = record.size();
            writeNative(bytes, programOffset, program);
        }
    }
    section.sh_size = record.size();
    writeNative(bytes, offset, section);
    return bytes;
}

auto recordMetadataError(const QByteArray &record, const QString &expected_iid) -> QString {
    return pluginMetadataError(metadataContainer(record), expected_iid);
}

auto metadataBytes(const QCborMap &map) -> QByteArray {
    QPluginMetaData::MagicHeader header;
    return QByteArray(reinterpret_cast<const char *>(&header), sizeof(header)) +
           QCborValue(map).toCbor();
}

auto elfMetadataPayload(const QByteArray &payload) -> QByteArray {
    QPluginMetaData::ElfNoteHeader note(payload.size());
    auto prefixSize = offsetof(QPluginMetaData::ElfNoteHeader, header) + sizeof(note.header);
    return QByteArray(reinterpret_cast<const char *>(&note), prefixSize) + payload;
}

auto elfMetadataBytes(const QCborMap &map) -> QByteArray {
    return elfMetadataPayload(QCborValue(map).toCbor());
}

void rejectWithoutExecution(const QByteArray &bytes, const QString &sentinel_path,
                            const QString &error_fragment) {
    PluginLibrary library;
    if (library.load(bytes, QStringLiteral("rejected-abi-fixture.so")) ||
        library.instance() != nullptr || !library.errorString().contains(error_fragment)) {
        qFatal("native admission must reject before loading, expected %s, found %s",
               qPrintable(error_fragment), qPrintable(library.errorString()));
    }
    if (QFile::exists(sentinel_path)) {
        qFatal("native admission refusal must run neither initializer nor constructor");
    }
}

void redirectedSectionRejectionSmoke(const QByteArray &bytes, const QString &sentinel_path) {
    QCborMap root;
    root.insert(2, QStringLiteral("dev.silent.IPlugin/8.0"));
    auto replacement = elfMetadataBytes(root);
    auto offset = sectionHeaderOffset(bytes, ".note.qt.metadata");
    auto section = readNative<Elf64_Shdr>(bytes, offset);
    auto redirected = bytes;
    section.sh_offset = (bytes.size() + 7) & ~quint64(7);
    section.sh_size = replacement.size();
    writeNative(redirected, offset, section);
    redirected.append(QByteArray(section.sh_offset - redirected.size(), '\0'));
    redirected.append(replacement);
    auto header = readNative<Elf64_Ehdr>(bytes, 0);
    if (redirected.mid(header.e_phoff, header.e_phnum * header.e_phentsize) !=
        bytes.mid(header.e_phoff, header.e_phnum * header.e_phentsize)) {
        qFatal("section redirection regression must leave mapped ABI 7 program headers unchanged");
    }
    rejectWithoutExecution(redirected, sentinel_path, QStringLiteral("mapped native image"));
    section.sh_addr = section.sh_offset;
    writeNative(redirected, offset, section);
    rejectWithoutExecution(redirected, sentinel_path, QStringLiteral("mapped native image"));
}

void metadataMappingRejectionSmoke(const QByteArray &bytes, const QString &sentinel_path) {
    auto offset = sectionHeaderOffset(bytes, ".note.qt.metadata");
    auto original = readNative<Elf64_Shdr>(bytes, offset);
    for (int variant = 0; variant < 5; ++variant) {
        auto section = original;
        switch (variant) {
        case 0:
            section.sh_flags &= ~quint64(SHF_ALLOC);
            break;
        case 1:
            section.sh_flags |= SHF_WRITE;
            break;
        case 2:
            section.sh_flags |= SHF_EXECINSTR;
            break;
        case 3:
            section.sh_addr += 8;
            break;
        case 4:
            section.sh_addralign = 3;
            break;
        }
        auto invalid = bytes;
        writeNative(invalid, offset, section);
        rejectWithoutExecution(invalid, sentinel_path, QStringLiteral("metadata"));
    }
    auto header = readNative<Elf64_Ehdr>(bytes, 0);
    auto gnuNote = readNative<Elf64_Shdr>(bytes, sectionHeaderOffset(bytes, ".note.gnu.build-id"));
    auto wrongNote = bytes;
    for (int index = 0; std::cmp_less(index, header.e_phnum); ++index) {
        auto programOffset = header.e_phoff + index * header.e_phentsize;
        auto program = readNative<Elf64_Phdr>(bytes, programOffset);
        if (program.p_type == PT_NOTE && program.p_offset == original.sh_offset) {
            program.p_offset = gnuNote.sh_offset;
            program.p_vaddr = gnuNote.sh_addr;
            program.p_filesz = gnuNote.sh_size;
            program.p_memsz = gnuNote.sh_size;
            program.p_align = gnuNote.sh_addralign;
            writeNative(wrongNote, programOffset, program);
        }
    }
    rejectWithoutExecution(wrongNote, sentinel_path, QStringLiteral("PT_NOTE location"));

    QCborMap innerRoot;
    innerRoot.insert(2, QStringLiteral("dev.silent.IPlugin/8.0"));
    auto innerNote = elfMetadataBytes(innerRoot);
    for (int padding = 0; padding < 8; ++padding) {
        QCborMap outerRoot;
        outerRoot.insert(2, QStringLiteral("dev.silent.IPlugin/7.0"));
        outerRoot.insert(4, QByteArray(padding, '\0') + innerNote);
        auto nested = metadataContainer(elfMetadataBytes(outerRoot));
        auto section = readNative<Elf64_Shdr>(nested, offset);
        auto innerOffset = nested.indexOf(innerNote, section.sh_offset);
        if (innerOffset < 0 || innerOffset % 8 != 0) {
            continue;
        }
        section.sh_addr += innerOffset - section.sh_offset;
        section.sh_offset = innerOffset;
        section.sh_size = innerNote.size();
        writeNative(nested, offset, section);
        rejectWithoutExecution(nested, sentinel_path, QStringLiteral("PT_NOTE location"));
        return;
    }
    qFatal("nested-note boundary regression requires one aligned location");
}

auto symbolOffset(const QByteArray &bytes, const QByteArray &name) -> qsizetype {
    auto symbols = readNative<Elf64_Shdr>(bytes, sectionHeaderOffset(bytes, ".dynsym"));
    auto strings = readNative<Elf64_Shdr>(bytes, sectionHeaderOffset(bytes, ".dynstr"));
    auto names = bytes.mid(strings.sh_offset, strings.sh_size);
    for (quint64 index = 0; index < symbols.sh_size / symbols.sh_entsize; ++index) {
        auto offset = symbols.sh_offset + index * symbols.sh_entsize;
        auto symbol = readNative<Elf64_Sym>(bytes, offset);
        if (std::cmp_less(symbol.st_name, names.size()) &&
            names.mid(symbol.st_name).startsWith(name + '\0')) {
            return offset;
        }
    }
    qFatal("required symbol absent from compiled fixture: %s", name.constData());
}

auto customGnuTable(const QByteArray &bytes, quint32 bloom_size, quint32 bucket_count = 1)
    -> QByteArray {
    auto symbols = readNative<Elf64_Shdr>(bytes, sectionHeaderOffset(bytes, ".dynsym"));
    auto strings = readNative<Elf64_Shdr>(bytes, sectionHeaderOffset(bytes, ".dynstr"));
    auto names = bytes.mid(strings.sh_offset, strings.sh_size);
    auto count = symbols.sh_size / symbols.sh_entsize;
    auto bucketOffset = 16 + bloom_size * sizeof(quint64);
    auto chainOffset = bucketOffset + bucket_count * sizeof(quint32);
    QByteArray table(chainOffset + (count - 1) * sizeof(quint32), '\0');
    writeNative(table, 0, bucket_count);
    writeNative(table, 4, quint32(1));
    writeNative(table, 8, bloom_size);
    writeNative(table, 12, quint32(6));
    table.replace(16, bloom_size * sizeof(quint64),
                  QByteArray(bloom_size * sizeof(quint64), char(0xff)));
    writeNative(table, bucketOffset, quint32(1));
    for (quint64 index = 1; index < count; ++index) {
        auto symbol = readNative<Elf64_Sym>(bytes, symbols.sh_offset + index * symbols.sh_entsize);
        auto end = names.indexOf('\0', symbol.st_name);
        if (end < 0) {
            qFatal("compiled GNU hash fixture symbol name is unterminated");
        }
        auto name = names.mid(symbol.st_name, end - symbol.st_name);
        auto hash = quint32(5381);
        for (auto byte : name) {
            hash = hash * 33 + quint8(byte);
        }
        writeNative(table, chainOffset + (index - 1) * sizeof(quint32),
                    (hash & ~quint32(1)) | quint32(index == count - 1));
    }
    return table;
}

auto withGnuTable(const QByteArray &bytes, const QByteArray &table) -> QByteArray {
    auto section = readNative<Elf64_Shdr>(bytes, sectionHeaderOffset(bytes, ".gnu.hash"));
    if (std::cmp_greater(table.size(), section.sh_size)) {
        qFatal("custom GNU table exceeds mapped fixture hash section");
    }
    auto patched = bytes;
    patched.replace(section.sh_offset, table.size(), table);
    return patched;
}

void gnuHashChildCase(const QByteArray &bytes, const QByteArray &table, const QString &name,
                      bool expected_admission) {
    QTemporaryDir directory;
    if (!directory.isValid()) {
        qFatal("GNU hash child directory creation failed");
    }
    auto patched = withGnuTable(bytes, table);
    auto path = directory.filePath(QStringLiteral("gnu-hash-fixture.so"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(patched) != patched.size()) {
        qFatal("GNU hash child fixture write failed");
    }
    file.close();
    auto sentinel = directory.filePath(QStringLiteral("execution-sentinel"));
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SILENT_ABI_SENTINEL"), sentinel);
    QProcess child;
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--plugin-gnu-hash-child"), path,
                 expected_admission ? QStringLiteral("admit") : QStringLiteral("reject")});
    if (!child.waitForStarted(30000) || !child.waitForFinished(30000)) {
        child.kill();
        child.waitForFinished();
        qFatal("GNU hash child timed out: %s", qPrintable(name));
    }
    auto output = child.readAllStandardOutput();
    auto errors = child.readAllStandardError();
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0 ||
        !output.contains(expected_admission ? "GNU hash child admitted"
                                            : "GNU hash child rejected")) {
        qFatal("GNU hash child %s failed: exit=%d status=%d stdout=%s stderr=%s", qPrintable(name),
               child.exitCode(), int(child.exitStatus()), output.constData(), errors.constData());
    }
    if (QFile::exists(sentinel) != expected_admission) {
        qFatal("GNU hash child execution sentinel disagrees with admission: %s", qPrintable(name));
    }
}

void gnuHashChildSmoke(const QByteArray &bytes) {
    gnuHashChildCase(bytes, customGnuTable(bytes, 4), QStringLiteral("valid-power-of-two"), true);
    gnuHashChildCase(bytes, customGnuTable(bytes, 3), QStringLiteral("bloom-size-three"), false);
    for (auto shift : {quint32(32), quint32(64), quint32(0xffffffff)}) {
        auto invalid = customGnuTable(bytes, 4);
        writeNative(invalid, 12, shift);
        gnuHashChildCase(bytes, invalid, QStringLiteral("bad-bloom-shift"), false);
    }
    auto unterminated = customGnuTable(bytes, 4);
    auto last = readNative<quint32>(unterminated, unterminated.size() - sizeof(quint32));
    writeNative(unterminated, unterminated.size() - sizeof(quint32), last & ~quint32(1));
    gnuHashChildCase(bytes, unterminated, QStringLiteral("unterminated-chain"), false);
    auto incongruent = customGnuTable(bytes, 4);
    writeNative(incongruent, incongruent.size() - sizeof(quint32), last ^ quint32(2));
    gnuHashChildCase(bytes, incongruent, QStringLiteral("incongruent-chain-hash"), false);
    auto outside = customGnuTable(bytes, 4, 2);
    auto symbols = readNative<Elf64_Shdr>(bytes, sectionHeaderOffset(bytes, ".dynsym"));
    writeNative(outside, 16 + 4 * sizeof(quint64) + sizeof(quint32),
                quint32(symbols.sh_size / symbols.sh_entsize + 10));
    gnuHashChildCase(bytes, outside, QStringLiteral("non-Qt-bucket-out-of-range"), false);
    for (auto offset : {quint32(0), quint32(symbols.sh_size / symbols.sh_entsize + 1)}) {
        auto invalid = customGnuTable(bytes, 4);
        writeNative(invalid, 4, offset);
        gnuHashChildCase(bytes, invalid, QStringLiteral("bad-symbol-offset"), false);
    }
    std::fprintf(stdout, "GNU hash child smoke: OK\n");
}

void entryExportRejectionSmoke(const QByteArray &bytes, const QString &sentinel_path) {
    auto strings = readNative<Elf64_Shdr>(bytes, sectionHeaderOffset(bytes, ".dynstr"));
    auto rodata = readNative<Elf64_Shdr>(bytes, sectionHeaderOffset(bytes, ".rodata"));
    for (const auto &name :
         {QByteArray("qt_plugin_instance"), QByteArray("qt_plugin_query_metadata_v2")}) {
        auto offset = symbolOffset(bytes, name);
        auto original = readNative<Elf64_Sym>(bytes, offset);
        auto missing = bytes;
        missing[strings.sh_offset + original.st_name] = 'x';
        missing = withGnuTable(missing, customGnuTable(missing, 4));
        rejectWithoutExecution(missing, sentinel_path,
                               QStringLiteral("missing defined Qt entry export"));
        for (int variant = 0; variant < 7; ++variant) {
            auto symbol = original;
            switch (variant) {
            case 0:
                symbol.st_shndx = SHN_UNDEF;
                break;
            case 1:
                symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
                break;
            case 2:
                symbol.st_other = STV_HIDDEN;
                break;
            case 3:
                symbol.st_info = ELF64_ST_INFO(STB_LOCAL, STT_FUNC);
                break;
            case 4:
                symbol.st_value = rodata.sh_addr;
                break;
            case 5:
                symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_GNU_IFUNC);
                break;
            case 6:
                symbol.st_shndx = SHN_ABS;
                break;
            }
            auto invalid = bytes;
            writeNative(invalid, offset, symbol);
            rejectWithoutExecution(invalid, sentinel_path, QStringLiteral("Qt "));
        }
    }
    auto redirected = bytes;
    auto offset = sectionHeaderOffset(bytes, ".dynsym");
    auto symbols = readNative<Elf64_Shdr>(bytes, offset);
    auto copy = bytes.mid(symbols.sh_offset, symbols.sh_size);
    symbols.sh_offset = (bytes.size() + 7) & ~quint64(7);
    writeNative(redirected, offset, symbols);
    redirected.append(QByteArray(symbols.sh_offset - redirected.size(), '\0'));
    redirected.append(copy);
    rejectWithoutExecution(redirected, sentinel_path, QStringLiteral("PT_DYNAMIC"));
    auto hash = readNative<Elf64_Shdr>(bytes, sectionHeaderOffset(bytes, ".gnu.hash"));
    auto bloomSize = readNative<quint32>(bytes, hash.sh_offset + 8);
    auto unreachable = bytes;
    unreachable.replace(hash.sh_offset + 16, bloomSize * sizeof(quint64),
                        QByteArray(bloomSize * sizeof(quint64), '\0'));
    rejectWithoutExecution(unreachable, sentinel_path, QStringLiteral("runtime export hash"));
}

void duplicateKeysRejectionSmoke(const QString &sentinel_path) {
    auto iid =
        QByteArray::fromHex("02") + QCborValue(QStringLiteral("dev.silent.IPlugin/8.0")).toCbor();
    for (const auto &payload :
         {QByteArray::fromHex("a3") + iid + QByteArray::fromHex("04a004a0"),
          QByteArray::fromHex("a3") + iid + QByteArray::fromHex("036161036162"),
          QByteArray::fromHex("a2") + iid + QByteArray::fromHex("04a201010102"),
          QByteArray::fromHex("a2") + iid + QByteArray::fromHex("04a2617801617802"),
          QByteArray::fromHex("a2") + iid + QByteArray::fromHex("04a1616181a2617801617802"),
          QByteArray::fromHex("a2") + iid + QByteArray::fromHex("04c0a2617801617802"),
          QByteArray::fromHex("a2") + iid + QByteArray::fromHex("04a1a261780161780200")}) {
        rejectWithoutExecution(metadataContainer(elfMetadataPayload(payload)), sentinel_path,
                               QStringLiteral("duplicate CBOR map key"));
    }
    auto deep = QByteArray::fromHex("a2") + iid + QByteArray::fromHex("04") +
                QByteArray(40, char(0x81)) + QByteArray(1, '\0');
    rejectWithoutExecution(metadataContainer(elfMetadataPayload(deep)), sentinel_path,
                           QStringLiteral("nesting depth exceeded"));
    auto many = QByteArray::fromHex("a2") + iid + QByteArray::fromHex("049f") +
                QByteArray(20000, '\0') + QByteArray(1, char(0xff));
    rejectWithoutExecution(metadataContainer(elfMetadataPayload(many)), sentinel_path,
                           QStringLiteral("item budget exceeded"));
}

void metadataCandidateSmoke() {
    auto expected = QStringLiteral("dev.silent.IPlugin/8.0");
    QCborMap root;
    root.insert(2, expected);
    auto regular = metadataBytes(root);
    auto elf = elfMetadataBytes(root);
    auto description =
        QJsonDocument::fromJson(
            R"({"description":"QTMETADATA ! and qt-project!\u0000 are ordinary text"})")
            .object();
    root.insert(4, QCborMap::fromJsonObject(description));
    for (const auto &record : {metadataBytes(root), elfMetadataBytes(root)}) {
        if (!recordMetadataError(record, expected).isEmpty()) {
            qFatal("Qt marker text in JSON metadata descriptions must be accepted");
        }
    }
    auto magicText =
        QByteArray(QPluginMetaData::MagicString, sizeof(QPluginMetaData::MagicString)) +
        QByteArray(" ordinary text");
    auto noteText = QByteArray(offsetof(QPluginMetaData::ElfNoteHeader, name), '\0') +
                    QByteArray(QPluginMetaData::ElfNoteHeader::NoteName,
                               sizeof(QPluginMetaData::ElfNoteHeader::NoteName)) +
                    QByteArray(sizeof(QPluginMetaData::Header), '\0');
    for (const auto &record : {regular, elf}) {
        if (pluginMetadataError(magicText + noteText + record + noteText + magicText, expected)
                .isEmpty()) {
            qFatal("unbound marker strings must not be admitted as a native library");
        }
    }
    QCborMap oldRoot;
    oldRoot.insert(2, QStringLiteral("dev.silent.IPlugin/4.0"));
    for (const auto &duplicate :
         {regular + regular, elf + elf, regular + elf, elf + regular,
          metadataBytes(oldRoot) + regular, regular + metadataBytes(oldRoot),
          elfMetadataBytes(oldRoot) + elf, elf + elfMetadataBytes(oldRoot)}) {
        if (recordMetadataError(duplicate, expected).isEmpty()) {
            qFatal("independent metadata records must be rejected regardless of their root IID");
        }
    }
    auto invalidBounds = elf;
    invalidBounds.replace(offsetof(QPluginMetaData::ElfNoteHeader, n_descsz), sizeof(quint32),
                          QByteArray(sizeof(quint32), char(0xff)));
    auto invalidVersion = regular;
    invalidVersion[sizeof(QPluginMetaData::MagicString)] = char(0xff);
    auto invalidCbor = regular;
    invalidCbor[sizeof(QPluginMetaData::MagicHeader)] = char(0xff);
    QPluginMetaData::ElfNoteHeader oversizedNote(QCborValue(root).toCbor().size() + regular.size());
    auto oversized = QByteArray(reinterpret_cast<const char *>(&oversizedNote),
                                offsetof(QPluginMetaData::ElfNoteHeader, header) +
                                    sizeof(oversizedNote.header)) +
                     QCborValue(root).toCbor() + regular;
    for (const auto &invalid : {invalidBounds, invalidVersion, invalidCbor, oversized}) {
        if (recordMetadataError(invalid, expected).isEmpty() ||
            recordMetadataError(invalid + regular, expected).isEmpty() ||
            recordMetadataError(regular + invalid, expected).isEmpty()) {
            qFatal("malformed recognized records must fail even beside valid metadata");
        }
    }
    root.insert(4, metadataBytes(oldRoot) + elfMetadataBytes(oldRoot));
    for (const auto &record : {metadataBytes(root), elfMetadataBytes(root)}) {
        if (!recordMetadataError(record, expected).isEmpty()) {
            qFatal("complete metadata records embedded inside a validated payload are only data");
        }
    }
    oldRoot.insert(4, regular);
    if (recordMetadataError(metadataBytes(oldRoot), expected).isEmpty()) {
        qFatal("nested ABI 8 metadata must not override the ABI 4 root IID");
    }
    QPluginMetaData::MagicHeader header;
    header.header.qt_minor_version = 6;
    auto qt66 = QByteArray(reinterpret_cast<const char *>(&header), sizeof(header)) +
                QCborValue(root).toCbor();
    if (!recordMetadataError(qt66, expected).isEmpty()) {
        qFatal("Qt 6.6.3 metadata must remain accepted on supported hosts");
    }
    header.header.qt_minor_version = 8;
    auto qt68 = QByteArray(reinterpret_cast<const char *>(&header), sizeof(header)) +
                QCborValue(root).toCbor();
    if (recordMetadataError(qt68, expected).isEmpty() != (QT_VERSION_MINOR >= 8)) {
        qFatal("Qt 6.8.2 metadata must be accepted only by a sufficiently new Qt host");
    }
}

void metadataRejectionSmoke(const QByteArray &bytes, const QString &sentinel_path) {
    auto expected = QStringLiteral("dev.silent.IPlugin/8.0");
    if (pluginMetadataError(bytes, QStringLiteral("dev.silent.IPlugin/4.0")).isEmpty()) {
        qFatal("ABI 4 expectation must reject an ABI 8 library");
    }
    QCborMap nested;
    nested.insert(QStringLiteral("IID"), expected);
    QCborMap metadata;
    metadata.insert(4, nested);
    if (recordMetadataError(metadataBytes(metadata), expected).isEmpty()) {
        qFatal("nested metadata IID must not satisfy the root IID check");
    }
    metadata.insert(2, expected);
    auto valid = metadataBytes(metadata);
    if (!recordMetadataError(valid, expected).isEmpty()) {
        qFatal("regular Qt metadata with the exact root IID must be accepted");
    }
    if (recordMetadataError(valid + valid, expected).isEmpty()) {
        qFatal("duplicate Qt metadata records must be rejected");
    }
    QPluginMetaData::MagicHeader header;
    auto prefix = QByteArray(reinterpret_cast<const char *>(&header), sizeof(header));
    auto iidEntry = QByteArray::fromHex("02") + QCborValue(expected).toCbor();
    auto duplicateIid = prefix + QByteArray::fromHex("a2") + iidEntry + iidEntry;
    if (recordMetadataError(duplicateIid, expected).isEmpty()) {
        qFatal("duplicate root IID keys must be rejected");
    }
    if (recordMetadataError(prefix, expected).isEmpty() ||
        recordMetadataError(valid.first(valid.size() - 1), expected).isEmpty()) {
        qFatal("truncated metadata header or CBOR must be rejected");
    }
    metadata.insert(2, expected + QStringLiteral("extra"));
    if (recordMetadataError(metadataBytes(metadata), expected).isEmpty()) {
        qFatal("root IID must match exactly, not just its ABI prefix");
    }
    metadata.insert(2, 4);
    if (recordMetadataError(metadataBytes(metadata), expected).isEmpty()) {
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
    auto orphan = missing + valid;
    auto prefixed = valid + bytes;
    auto wrongMachine = bytes;
    wrongMachine[offsetof(Elf64_Ehdr, e_machine)] = char(0xff);
    auto invalidSections = bytes;
    invalidSections.replace(offsetof(Elf64_Ehdr, e_shoff), sizeof(quint64),
                            QByteArray(sizeof(quint64), char(0xff)));
    auto incompatibleQt = bytes;
    incompatibleQt[headerOffset + offsetof(QPluginMetaData::Header, qt_major_version)] = char(5);
    auto incompatibleCpu = bytes;
    incompatibleCpu[headerOffset + offsetof(QPluginMetaData::Header, plugin_arch_requirements)] =
        char(0x7f);
    for (const auto &invalid :
         {missing, malformed, invalidCbor, orphan, prefixed, wrongMachine, invalidSections,
          incompatibleQt, incompatibleCpu, malformed + valid, invalidCbor + valid}) {
        rejectWithoutExecution(invalid, sentinel_path,
                               QStringLiteral("expected dev.silent.IPlugin/8.0"));
    }
    for (const auto &invalid : {malformed, invalidCbor}) {
        if (pluginMetadataError(invalid + valid, expected).isEmpty() ||
            pluginMetadataError(valid + invalid, expected).isEmpty()) {
            qFatal("a valid record must not override malformed recognized metadata");
        }
    }
}
} // namespace

auto pluginGnuHashChild(const QString &path, bool expected_admission) -> int {
    auto bytes = fixtureBytes(qPrintable(path));
    auto sentinelPath = QString::fromLocal8Bit(qgetenv("SILENT_ABI_SENTINEL"));
    if (sentinelPath.isEmpty() || QFile::exists(sentinelPath)) {
        return 1;
    }
    PluginLibrary library;
    auto loaded = library.load(bytes, QStringLiteral("gnu-hash-child.so"));
    if (!expected_admission) {
        if (loaded || library.instance() != nullptr || QFile::exists(sentinelPath) ||
            !library.errorString().contains(QStringLiteral("export hash"))) {
            std::fprintf(stderr, "GNU rejection failed: %s\n", qPrintable(library.errorString()));
            return 1;
        }
        std::fprintf(stdout, "GNU hash child rejected: %s\n", qPrintable(library.errorString()));
        return 0;
    }
    QFile sentinel(sentinelPath);
    if (!loaded || library.instance() == nullptr || !sentinel.open(QIODevice::ReadOnly) ||
        sentinel.readAll() != QByteArray("initializer\nconstructor\n")) {
        std::fprintf(stderr, "GNU valid control failed: %s\n", qPrintable(library.errorString()));
        return 1;
    }
    std::fprintf(stdout, "GNU hash child admitted\n");
    return 0;
}

void pluginAbiSmoke() {
    metadataCandidateSmoke();
    for (auto version : {2, 3, 4, 5, 6, 7, 8, 9}) {
        auto legacy = version != 8;
        QTemporaryDir directory;
        if (!directory.isValid()) {
            qFatal("ABI fixture directory creation failed");
        }
        auto sentinelPath = directory.filePath(QStringLiteral("execution-sentinel"));
        auto previousSentinel = qgetenv("SILENT_ABI_SENTINEL");
        auto hadSentinel = qEnvironmentVariableIsSet("SILENT_ABI_SENTINEL");
        qputenv("SILENT_ABI_SENTINEL", QFile::encodeName(sentinelPath));
        auto id = QStringLiteral("abi%1").arg(version);
        auto library = QString::fromUtf8(SILENT_ABI8_FIXTURE_PATH);
        if (version == 2) {
            library = QString::fromUtf8(SILENT_ABI2_FIXTURE_PATH);
        } else if (version == 3) {
            library = QString::fromUtf8(SILENT_ABI3_FIXTURE_PATH);
        } else if (version == 4) {
            library = QString::fromUtf8(SILENT_ABI4_FIXTURE_PATH);
        } else if (version == 5) {
            library = QString::fromUtf8(SILENT_ABI5_FIXTURE_PATH);
        } else if (version == 6) {
            library = QString::fromUtf8(SILENT_ABI6_FIXTURE_PATH);
        } else if (version == 7) {
            library = QString::fromUtf8(SILENT_ABI7_FIXTURE_PATH);
        } else if (version == 9) {
            library = QString::fromUtf8(SILENT_ABI9_FIXTURE_PATH);
        }
        auto libraryPath = QDir(directory.path()).filePath(QStringLiteral("lib%1.so").arg(id));
        if (!QFile::copy(library, libraryPath)) {
            qFatal("ABI fixture library copy failed");
        }
        if (!legacy) {
            QFile fixture(libraryPath);
            if (!fixture.open(QIODevice::ReadOnly)) {
                qFatal("ABI 8 fixture read failed");
            }
            auto bytes = fixture.readAll();
            metadataRejectionSmoke(bytes, sentinelPath);
            metadataMappingRejectionSmoke(bytes, sentinelPath);
            entryExportRejectionSmoke(bytes, sentinelPath);
            gnuHashChildSmoke(bytes);
            duplicateKeysRejectionSmoke(sentinelPath);
            if (QFile::exists(sentinelPath)) {
                qFatal("rejected metadata must run neither initializer nor constructor");
            }
        }
        if (version == 7) {
            redirectedSectionRejectionSmoke(fixtureBytes(SILENT_ABI7_FIXTURE_PATH), sentinelPath);
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
                qFatal("registry must reject ABI 2/3/4/5/6/7/9 and accept ABI 8 binaries");
            }
            if (legacy && (!registry.pluginLoadError(id).contains(
                               QStringLiteral("expected dev.silent.IPlugin/8.0")) ||
                           !registry.pluginLoadError(id).contains(
                               QStringLiteral("found dev.silent.IPlugin/%1.0").arg(version)))) {
                qFatal("incompatible ABI binary must fail at the metadata admission gate");
            }
            if (!registry.enabledModulesForLauncher().empty()) {
                qFatal("ABI fixture must register no modules or feed instances");
            }
        }
        QFile sentinel(sentinelPath);
        if (legacy) {
            if (sentinel.exists()) {
                qFatal("ABI 2/3/4/5/6/7/9 rejection must run neither initializer nor constructor");
            }
        } else if (!sentinel.open(QIODevice::ReadOnly) ||
                   sentinel.readAll() != QByteArray("initializer\nconstructor\n")) {
            qFatal("ABI 8 admission must run the library initializer and root constructor once");
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
