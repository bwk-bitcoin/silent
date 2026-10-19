#pragma once

#include <QHash>
#include <QObject>
#include <QTranslator>

class QApplication;

namespace i18n {

class I18nManager : public QObject {
    Q_OBJECT

public:
    static auto get() -> I18nManager *;

    void init(QApplication *app);
    [[nodiscard]] auto selectedLocale() const -> QString;
    [[nodiscard]] auto applyLocale(const QString &locale, bool persist = true) -> bool;
    auto registerPluginTranslations(const QString &plugin_id, const QString &dir) -> bool;
    // A plugin's catalogs held in memory, by file name (silent_en.lang): an external
    // plugin's, opened from the app state.
    auto registerPluginCatalogs(const QString &plugin_id, const QHash<QString, QString> &catalogs)
        -> bool;
    void unregisterPluginTranslations(const QString &plugin_id);

signals:
    void languageChanged(const QString &locale, bool success);

private:
    static I18nManager *s_instance; // NOLINT

    QApplication *m_app = nullptr;
    QTranslator *m_base_translator = nullptr;
    QTranslator *m_translator = nullptr;
    QString m_current_locale;
    bool m_base_translator_installed = false;
    bool m_translator_installed = false;

    struct PluginTranslatorEntry {
        QTranslator *translator = nullptr;
        QString plugin_id;
        QString dir;
        // Catalogs held in memory, by file name (silent_en.lang), read before `dir`.
        QHash<QString, QString> catalogs;
        bool installed = false;
    };

    QHash<QString, PluginTranslatorEntry> m_plugin_translators;
};

} // namespace i18n
