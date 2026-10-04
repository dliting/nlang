/*--- SettingsStore.h - persisted nide settings (language, build output) ---*/
#ifndef NLANG_TOOLS_NIDE_SETTINGS_STORE_H
#define NLANG_TOOLS_NIDE_SETTINGS_STORE_H

#include <QLocale>
#include <QString>
#include <QStringList>

class QSettings;

namespace nlang {

//Stored language values: follow the system, or a pinned tree.
inline const QString LANGUAGE_SYSTEM = QStringLiteral("system");
inline const QString LANGUAGE_ZH = QStringLiteral("zh");
inline const QString LANGUAGE_EN = QStringLiteral("en");
inline const QString TOOLBAR_ICON_SMALL = QStringLiteral("small");
inline const QString TOOLBAR_ICON_LARGE = QStringLiteral("large");

//The Tools > Options values. No UI: SettingsDialog edits the form,
//this persists (QSettings org/app from main.cpp). The value surface is
//deliberately two strings -- every derived decision (translator
//locale, help tree, .ncu placement) lives in a pure static resolver
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
    //"" = disabled: standalone .ncu files keep the per-user temp area
    //and projects fall back to the project directory.
    QString buildOutputDir() const { return m_buildOutputDir; }
    void setBuildOutputDir(const QString& dir) { m_buildOutputDir = dir; }
    //"small" (default, 32px) | "large" (48px) toolbar icons.
    QString toolbarIconSize() const { return m_toolbarIconSize; }
    void setToolbarIconSize(const QString& v) { m_toolbarIconSize = v; }
   //Global library search dirs (Tools > Options): passed as -I to the
    //tools and indexed for code assistance. Project paths take precedence.
    QStringList librarySearchPaths() const { return m_librarySearchPaths; }
    void setLibrarySearchPaths(const QStringList& dirs)
        { m_librarySearchPaths = dirs; }
    //0.7.5: the compiler-options group's first member -- pass --no-warn
    //to ncc. Global level; a project's own opt-in adds on top (see
    //ProjectNode::noWarn).
    bool noWarn() const { return m_noWarn; }
    void setNoWarn(bool v) { m_noWarn = v; }

    //Locale handed to installTranslations.
    QLocale languageLocale() const
        { return localeForLanguage(m_language); }
    //Docs-site tree for the help viewer ("zh" | "en" -- never
    //"system": that resolves against the system locale).
    QString helpTree() const { return helpTreeForLanguage(m_language); }

    static QLocale localeForLanguage(const QString& language);
    static QString helpTreeForLanguage(const QString& language);
    //The per-user directory standalone .ncu files land in when the
    //global setting is empty; the Options dialog pre-fills it as the
    //visible default.
    static QString defaultStandaloneBuildDir();
    //One .ncu slot per source stem: in the global build output
    //directory when set, else the per-user temp area (today's layout).
    static QString resolveStandaloneNcuPath(const QString& buildOutputDir,
                                             const QString& sourcePath);
    //The project artifact's path: explicit .nproj outputDir wins, then
    //the global build output directory, then the project directory.
    //The artifact is the .npkg package (project builds pack one member
    //per unit; phase 6).
    static QString resolveProjectPackagePath(const QString& projectOutputDir,
                                             const QString& projectDir,
                                             const QString& buildOutputDir,
                                             const QString& projectName);

private:
    QString m_language = LANGUAGE_SYSTEM;
    QString m_buildOutputDir;
    QString m_toolbarIconSize = TOOLBAR_ICON_SMALL;
   QStringList m_librarySearchPaths;
    bool m_noWarn = false;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_SETTINGS_STORE_H
