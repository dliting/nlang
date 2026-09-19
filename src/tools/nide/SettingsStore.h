/*--- SettingsStore.h - persisted nide settings (language, build output) ---*/
#ifndef NLANG_TOOLS_NIDE_SETTINGS_STORE_H
#define NLANG_TOOLS_NIDE_SETTINGS_STORE_H

#include <QLocale>
#include <QString>

class QSettings;

namespace nlang {

//Stored language values: follow the system, or a pinned tree.
inline const QString LANGUAGE_SYSTEM = QStringLiteral("system");
inline const QString LANGUAGE_ZH = QStringLiteral("zh");
inline const QString LANGUAGE_EN = QStringLiteral("en");

//The Tools > Options values. No UI: SettingsDialog edits the form,
//this persists (QSettings org/app from main.cpp). The value surface is
//deliberately two strings -- every derived decision (translator
//locale, help tree, .nmod placement) lives in a pure static resolver
//so tests need no QSettings.
class SettingsStore {
public:
    //Load/persist through an injected QSettings (tests use a temp ini).
    void load(QSettings& settings);
    void save(QSettings& settings) const;
    //Convenience over the default QSettings (org/app from main.cpp).
    static SettingsStore persisted();
    void persist() const;

    //"system" (default) | "zh" | "en".
    QString language() const { return m_language; }
    void setLanguage(const QString& language) { m_language = language; }
    //"" = disabled: standalone .nmod files keep the per-user temp area
    //and projects fall back to the project directory.
    QString buildOutputDir() const { return m_buildOutputDir; }
    void setBuildOutputDir(const QString& dir) { m_buildOutputDir = dir; }

    //Locale handed to installTranslations.
    QLocale languageLocale() const
        { return localeForLanguage(m_language); }
    //Docs-site tree for the help viewer ("zh" | "en" -- never
    //"system": that resolves against the system locale).
    QString helpTree() const { return helpTreeForLanguage(m_language); }

    static QLocale localeForLanguage(const QString& language);
    static QString helpTreeForLanguage(const QString& language);
    //The per-user directory standalone .nmod files land in when the
    //global setting is empty; the Options dialog pre-fills it as the
    //visible default.
    static QString defaultStandaloneBuildDir();
    //One .nmod slot per source stem: in the global build output
    //directory when set, else the per-user temp area (today's layout).
    static QString resolveStandaloneNmodPath(const QString& buildOutputDir,
                                             const QString& sourcePath);
    //Explicit .nproj outputDir wins, then the global build output
    //directory, then the project directory.
    static QString resolveProjectNmodPath(const QString& projectOutputDir,
                                          const QString& projectDir,
                                          const QString& buildOutputDir,
                                          const QString& projectName);

private:
    QString m_language = LANGUAGE_SYSTEM;
    QString m_buildOutputDir;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_SETTINGS_STORE_H
