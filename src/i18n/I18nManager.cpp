#include "I18nManager.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QLoggingCategory>
#include <QTextStream>
#include <silent.h>

namespace i18n {

namespace {
auto unescapeText(const QString &value) -> QString {
    QString out;
    out.reserve(value.size());
    for (int i = 0; i < value.size(); ++i) {
        QChar c = value[i];
        if (c != '\\' || (i + 1) >= value.size()) {
            out += c;
            continue;
        }
        QChar next = value[i + 1];
        if (next == 'n') {
            out += '\n';
        } else if (next == 't') {
            out += '\t';
        } else {
            out += next;
        }
        ++i;
    }
    return out;
}

auto parseQuotedValues(const QString &line) -> QStringList {
    QStringList values;
    bool inQuote = false;
    bool escaped = false;
    QString current;
    for (QChar c : line) {
        if (!inQuote) {
            if (c == '"') {
                inQuote = true;
                current.clear();
            }
            continue;
        }

        if (escaped) {
            current += '\\';
            current += c;
            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }

        if (c == '"') {
            values << unescapeText(current);
            inQuote = false;
            continue;
        }

        current += c;
    }
    return values;
}

class CatalogTranslator final : public QTranslator {
public:
    auto loadLangResource(const QString &resource_path, const QString &id_prefix = QString())
        -> bool {
        m_messages.clear();

        QFile file(resource_path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return false;
        }

        QTextStream input(&file);
        QString currentId;
        QStringList blockValues;
        while (!input.atEnd()) {
            QString raw = input.readLine();
            QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith('#')) {
                continue;
            }

            if (line.contains("=>")) {
                if (!currentId.isEmpty()) {
                    bool end = line.endsWith(";;");
                    auto values = parseQuotedValues(line);
                    for (const auto &value : values) {
                        blockValues << value;
                    }
                    if (end) {
                        if (!blockValues.isEmpty()) {
                            QString picked = blockValues.last();
                            if (picked != "NONE") {
                                m_messages.insert(id_prefix + currentId, picked);
                            }
                        }
                        currentId.clear();
                        blockValues.clear();
                    }
                    continue;
                }

                int idSep = line.indexOf("=>");
                if (idSep <= 0) {
                    return false;
                }
                QString msgId = line.left(idSep).trimmed();
                QString rest = line.mid(idSep + 2).trimmed();
                auto values = parseQuotedValues(rest);
                if (values.isEmpty() || values.size() > 2) {
                    return false;
                }
                QString picked = values.last();
                if (picked != "NONE") {
                    m_messages.insert(id_prefix + msgId, picked);
                }
                continue;
            }

            if (!currentId.isEmpty()) {
                return false;
            }
            currentId = line;
            blockValues.clear();
        }

        return true;
    }

    auto translate(const char *context, const char *source_text, const char *disambiguation,
                   int n) const -> QString override {
        Q_UNUSED(context);
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        QString key = QString::fromUtf8(source_text);
        auto it = m_messages.constFind(key);
        if (it == m_messages.constEnd()) {
            return QString();
        }
        return *it;
    }

    // QTranslator reports itself empty unless it loaded a .qm file, and Qt neither
    // installs an empty translator nor sends LanguageChange for it.
    [[nodiscard]] auto isEmpty() const -> bool override {
        return m_messages.isEmpty();
    }

private:
    QHash<QString, QString> m_messages;
};

auto resolveCandidateLocales(const QString &locale) -> QStringList {
    QStringList candidates;
    auto normalized = locale.trimmed();
    if (!normalized.isEmpty()) {
        normalized.replace('-', '_');
        candidates << normalized;

        int sep = normalized.indexOf('_');
        if (sep > 0) {
            candidates << normalized.left(sep);
        }
    }

    auto systemLocale = QLocale::system().name();
    if (!systemLocale.isEmpty()) {
        candidates << systemLocale;
        int sep = systemLocale.indexOf('_');
        if (sep > 0) {
            candidates << systemLocale.left(sep);
        }
    }

    candidates << "en";
    candidates.removeDuplicates();
    return candidates;
}
} // namespace

I18nManager *I18nManager::s_instance = nullptr;

auto I18nManager::get() -> I18nManager * {
    if (s_instance == nullptr) {
        s_instance = new I18nManager();
    }
    return s_instance;
}

void I18nManager::init(QApplication *app) {
    m_app = app;
    m_base_translator = new CatalogTranslator();
    m_translator = new CatalogTranslator();

    QString startupLocale = QString::fromStdString(std::string(app_language()));
    if (startupLocale.isEmpty()) {
        startupLocale = QStringLiteral("en");
    }
    bool loaded = applyLocale(startupLocale, false);
    qInfo() << "i18n init locale=" << startupLocale << " loaded=" << loaded;
}

auto I18nManager::selectedLocale() const -> QString {
    return m_current_locale;
}

auto I18nManager::applyLocale(const QString &locale, bool persist) -> bool {
    if (m_app == nullptr) {
        qWarning() << "i18n applyLocale called before init";
        return false;
    }

    auto candidates = resolveCandidateLocales(locale);

    if (m_base_translator_installed) {
        QApplication::removeTranslator(m_base_translator);
        m_base_translator_installed = false;
    }
    auto *baseTranslator = static_cast<CatalogTranslator *>(m_base_translator);
    if (baseTranslator != nullptr && baseTranslator->loadLangResource(":/i18n/silent_en.lang")) {
        m_base_translator_installed = QApplication::installTranslator(m_base_translator);
    }

    auto *localeTranslator = static_cast<CatalogTranslator *>(m_translator);
    QString loadedLocale;
    for (const auto &candidate : candidates) {
        auto resourcePath = QString(":/i18n/silent_%1.lang").arg(candidate);
        if (localeTranslator != nullptr && localeTranslator->loadLangResource(resourcePath)) {
            loadedLocale = candidate;
            break;
        }
    }

    bool success = m_base_translator_installed;
    if (!loadedLocale.isEmpty()) {
        if (m_translator_installed) {
            QApplication::removeTranslator(m_translator);
            m_translator_installed = false;
        }
        m_translator_installed = QApplication::installTranslator(m_translator);
        success = success || m_translator_installed;
        if (success) {
            m_current_locale = m_translator_installed ? loadedLocale : QStringLiteral("en");
        }
    } else {
        if (m_translator_installed) {
            QApplication::removeTranslator(m_translator);
            m_translator_installed = false;
        }
        m_current_locale = "en";
        if (!m_base_translator_installed) {
            qWarning() << "No embedded translation resource found for" << candidates;
        }
    }

    for (auto it = m_plugin_translators.begin(); it != m_plugin_translators.end(); ++it) {
        auto &entry = it.value();
        auto *pluginTranslator = static_cast<CatalogTranslator *>(entry.translator);
        if (pluginTranslator == nullptr) {
            continue;
        }

        if (entry.installed) {
            QApplication::removeTranslator(entry.translator);
            entry.installed = false;
        }

        bool pluginLoaded = false;
        const QString prefix = entry.plugin_id + '.';
        for (const auto &candidate : candidates) {
            auto filePath = QDir(entry.dir).filePath(QString("silent_%1.lang").arg(candidate));
            if (pluginTranslator->loadLangResource(filePath, prefix)) {
                pluginLoaded = true;
                break;
            }
        }

        if (pluginLoaded) {
            entry.installed = QApplication::installTranslator(entry.translator);
        }
    }

    if (persist) {
        app_set_language(rust::String(locale.toStdString()));
    }

    emit languageChanged(m_current_locale, success);
    qInfo() << "Applied locale request=" << locale << " active=" << m_current_locale
            << " success=" << success;
    return success;
}

auto I18nManager::registerPluginTranslations(const QString &plugin_id, const QString &dir) -> bool {
    if (m_app == nullptr) {
        qWarning() << "registerPluginTranslations called before init";
        return false;
    }

    QString normalizedId = plugin_id.trimmed();
    QString normalizedDir = dir.trimmed();
    if (normalizedId.isEmpty() || normalizedDir.isEmpty()) {
        return false;
    }

    unregisterPluginTranslations(normalizedId);

    PluginTranslatorEntry entry;
    entry.translator = new CatalogTranslator();
    entry.plugin_id = normalizedId;
    entry.dir = normalizedDir;

    m_plugin_translators.insert(normalizedId, entry);
    return applyLocale(m_current_locale.isEmpty() ? QStringLiteral("en") : m_current_locale, false);
}

void I18nManager::unregisterPluginTranslations(const QString &plugin_id) {
    QString normalizedId = plugin_id.trimmed();
    if (normalizedId.isEmpty() || !m_plugin_translators.contains(normalizedId)) {
        return;
    }

    auto entry = m_plugin_translators.take(normalizedId);
    if (entry.installed && entry.translator != nullptr) {
        QApplication::removeTranslator(entry.translator);
    }
    delete entry.translator;
}

} // namespace i18n
