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
} // namespace

void SettingsStore::load(QSettings& settings) {
    m_language = settings.value(LANGUAGE_KEY, LANGUAGE_SYSTEM).toString();
    m_buildOutputDir =
        settings.value(BUILD_OUTPUT_DIR_KEY).toString();
    m_toolbarIconSize =
        settings.value(TOOLBAR_ICON_SIZE_KEY, TOOLBAR_ICON_SMALL).toString();
}

void SettingsStore::save(QSettings& settings) const {
    settings.setValue(LANGUAGE_KEY, m_language);
    settings.setValue(BUILD_OUTPUT_DIR_KEY, m_buildOutputDir);
    settings.setValue(TOOLBAR_ICON_SIZE_KEY, m_toolbarIconSize);
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

QString SettingsStore::resolveStandaloneNmodPath(
    const QString& buildOutputDir, const QString& sourcePath) {
    const QString fileName =
        QFileInfo(sourcePath).completeBaseName()
        + QStringLiteral(".nmod");
    if (buildOutputDir.isEmpty())
        return QDir(defaultStandaloneBuildDir()).filePath(fileName);
    return QDir(buildOutputDir).filePath(fileName);
}

QString SettingsStore::resolveProjectNmodPath(
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
    return QDir(dir).filePath(projectName + QStringLiteral(".nmod"));
}

} // namespace nlang
