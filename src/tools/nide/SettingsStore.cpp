/*--- SettingsStore.cpp - persisted nide settings ---*/
#include "SettingsStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace nlang {

namespace {
const char* const LANGUAGE_KEY = "ide/language";
const char* const BUILD_OUTPUT_DIR_KEY = "ide/buildOutputDir";
const char* const TOOLBAR_ICON_SIZE_KEY = "ide/toolbarIconSize";
const char* const LIBRARY_SEARCH_PATHS_KEY = "ide/librarySearchPaths";
const char* const NO_WARN_KEY = "compiler/noWarn";
const char* const EDITOR_FONT_PT_KEY = "ide/editorFontPt";
const char* const TERMINAL_FONT_PT_KEY = "ide/terminalFontPt";
} // namespace

void SettingsStore::load(QSettings& settings) {
    m_language = settings.value(LANGUAGE_KEY, LANGUAGE_SYSTEM).toString();
    m_buildOutputDir =
        settings.value(BUILD_OUTPUT_DIR_KEY).toString();
    m_toolbarIconSize =
        settings.value(TOOLBAR_ICON_SIZE_KEY, TOOLBAR_ICON_SMALL).toString();
    m_librarySearchPaths =
        settings.value(LIBRARY_SEARCH_PATHS_KEY).toStringList();
    m_noWarn = settings.value(NO_WARN_KEY, false).toBool();
    //A stale or hand-edited stored value must land in range: every
    //editor seeds its font straight from here.
    m_editorFontPt = qBound(EDITOR_FONT_MIN_PT,
        settings.value(EDITOR_FONT_PT_KEY, EDITOR_FONT_DEFAULT_PT)
            .toInt(),
        EDITOR_FONT_MAX_PT);
    //Same clamp as the editor font: the terminal seeds straight from
    //here at startup.
    m_terminalFontPt = qBound(TERMINAL_FONT_MIN_PT,
        settings.value(TERMINAL_FONT_PT_KEY, TERMINAL_FONT_DEFAULT_PT)
            .toInt(),
        TERMINAL_FONT_MAX_PT);
}

void SettingsStore::save(QSettings& settings) const {
    settings.setValue(LANGUAGE_KEY, m_language);
    settings.setValue(BUILD_OUTPUT_DIR_KEY, m_buildOutputDir);
    settings.setValue(TOOLBAR_ICON_SIZE_KEY, m_toolbarIconSize);
    settings.setValue(LIBRARY_SEARCH_PATHS_KEY, m_librarySearchPaths);
    settings.setValue(NO_WARN_KEY, m_noWarn);
    settings.setValue(EDITOR_FONT_PT_KEY, m_editorFontPt);
    settings.setValue(TERMINAL_FONT_PT_KEY, m_terminalFontPt);
}

void SettingsStore::setEditorFontPt(int pt) {
    m_editorFontPt = qBound(EDITOR_FONT_MIN_PT, pt, EDITOR_FONT_MAX_PT);
}

void SettingsStore::setTerminalFontPt(int pt) {
    m_terminalFontPt =
        qBound(TERMINAL_FONT_MIN_PT, pt, TERMINAL_FONT_MAX_PT);
}

SettingsStore SettingsStore::persisted() {
    SettingsStore store;
    QSettings settings;
    store.load(settings);
    return store;
}

void SettingsStore::persist() const {
    QSettings settings;
    save(settings);
}

QLocale SettingsStore::localeForLanguage(const QString& language) {
    if (language == LANGUAGE_ZH)
        return QLocale(QLocale::Chinese);
    if (language == LANGUAGE_EN)
        return QLocale(QLocale::English);
    return QLocale();  // "system" (and garbage): the system locale
}

QString SettingsStore::helpTreeForLanguage(const QString& language) {
    if (language == LANGUAGE_ZH)
        return LANGUAGE_ZH;
    if (language == LANGUAGE_EN)
        return LANGUAGE_EN;
    //Anything else (incl. garbage) behaves like "system": the
    //machine's locale picks the tree.
    return QLocale().language() == QLocale::Chinese
        ? LANGUAGE_ZH : LANGUAGE_EN;
}

QString SettingsStore::defaultStandaloneBuildDir() {
    return QDir(QDir::temp()).filePath(QStringLiteral("nlang-nide"));
}

QString SettingsStore::resolveStandaloneNcuPath(
    const QString& buildOutputDir, const QString& sourcePath) {
    const QString fileName =
        QFileInfo(sourcePath).completeBaseName()
        + QStringLiteral(".ncu");
    if (buildOutputDir.isEmpty())
        return QDir(defaultStandaloneBuildDir()).filePath(fileName);
    return QDir(buildOutputDir).filePath(fileName);
}

QString SettingsStore::resolveProjectPackagePath(
    const QString& projectOutputDir, const QString& projectDir,
    const QString& buildOutputDir, const QString& projectName) {
    QString dir;
    if (!projectOutputDir.isEmpty())
        //QDir::filePath keeps an absolute second argument as-is, so
        //both .nproj spellings (relative/absolute) resolve here.
        dir = QFileInfo(QDir(projectDir).filePath(projectOutputDir))
                  .absoluteFilePath();
    else if (!buildOutputDir.isEmpty())
        dir = buildOutputDir;
    else
        dir = projectDir;
    //Project builds pack the .npkg package (one member per unit);
    //linking happens at load time (phase 6).
    return QDir(dir).filePath(projectName + QStringLiteral(".npkg"));
}

} // namespace nlang
