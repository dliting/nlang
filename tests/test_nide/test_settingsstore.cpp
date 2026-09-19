/*--- test_settingsstore.cpp - SettingsStore unit tests (no widgets) ---*/
#include "SettingsStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace nlang;

class TestSettingsStore : public QObject {
    Q_OBJECT

private slots:
    //The "system"/"zh"/"en" spellings appear as literals on purpose:
    //these tests pin the persisted strings, not the header constants.
    void testDefaults() {
        SettingsStore store;
        QSettings settings(iniPath(), QSettings::IniFormat);
        store.load(settings);
        QCOMPARE(store.language(), QString("system"));
        QVERIFY(store.buildOutputDir().isEmpty());
    }

    void testRoundTrip() {
        SettingsStore store;
        QSettings settings(iniPath(), QSettings::IniFormat);
        store.load(settings);
        store.setLanguage("zh");
        store.setBuildOutputDir("D:/dev/out");
        store.save(settings);

        SettingsStore reloaded;
        QSettings reloadedSettings(iniPath(), QSettings::IniFormat);
        reloaded.load(reloadedSettings);
        QCOMPARE(reloaded.language(), QString("zh"));
        QCOMPARE(reloaded.buildOutputDir(), QString("D:/dev/out"));
    }

    void testLocaleForLanguage() {
        QCOMPARE(SettingsStore::localeForLanguage("zh").language(),
                 QLocale::Chinese);
        QCOMPARE(SettingsStore::localeForLanguage("en").language(),
                 QLocale::English);
        //"system" and garbage fall back to the system locale.
        QCOMPARE(SettingsStore::localeForLanguage("system"),
                 QLocale());
        QCOMPARE(SettingsStore::localeForLanguage("klingon"),
                 QLocale());
    }

    void testHelpTreeForLanguage() {
        QCOMPARE(SettingsStore::helpTreeForLanguage("zh"),
                 QString("zh"));
        QCOMPARE(SettingsStore::helpTreeForLanguage("en"),
                 QString("en"));
        //"system" resolves against the machine locale; any other
        //locale reads the English tree (spec D5 mapping).
        QCOMPARE(SettingsStore::helpTreeForLanguage("system"),
                 QLocale().language() == QLocale::Chinese
                     ? QString("zh") : QString("en"));
    }

    void testDefaultStandaloneBuildDir() {
        //One shared source: the resolver's unset layout and the
        //Options dialog's prefilled default name the same directory.
        QCOMPARE(SettingsStore::defaultStandaloneBuildDir(),
                 QDir(QDir::temp()).filePath("nlang-nide"));
    }

    void testResolveStandaloneNmodPath() {
        //Empty setting keeps today's per-user temp layout (one slot
        //per source stem, completeBaseName semantics preserved).
        QCOMPARE(SettingsStore::resolveStandaloneNmodPath(
                     "", "D:/src/main.n"),
                 QDir(QDir::temp()).filePath("nlang-nide/main.nmod"));
        QCOMPARE(SettingsStore::resolveStandaloneNmodPath(
                     "", "D:/src/a.b.n"),
                 QDir(QDir::temp()).filePath("nlang-nide/a.b.nmod"));
        //A set directory redirects the slot there.
        QCOMPARE(SettingsStore::resolveStandaloneNmodPath(
                     "D:/dev/out", "D:/src/main.n"),
                 QString("D:/dev/out/main.nmod"));
    }

    void testResolveProjectNmodPath() {
        //Explicit .nproj outputDir wins (absolute spelling kept).
        QCOMPARE(SettingsStore::resolveProjectNmodPath(
                     "D:/explicit", "D:/proj", "D:/global", "App"),
                 QString("D:/explicit/App.nmod"));
        //A relative .nproj outputDir anchors at the project dir.
        QCOMPARE(SettingsStore::resolveProjectNmodPath(
                     "out", "D:/proj", "D:/global", "App"),
                 QString("D:/proj/out/App.nmod"));
        //Unset .nproj + global setting: the global directory.
        QCOMPARE(SettingsStore::resolveProjectNmodPath(
                     "", "D:/proj", "D:/global", "App"),
                 QString("D:/global/App.nmod"));
        //Everything unset: the project directory (today's behavior).
        QCOMPARE(SettingsStore::resolveProjectNmodPath(
                     "", "D:/proj", "", "App"),
                 QString("D:/proj/App.nmod"));
    }

private:
    //A fresh ini per test function: no cross-test bleed.
    QString iniPath() const {
        return m_dir.filePath(
            QString::fromLatin1(QTest::currentTestFunction()) + ".ini");
    }
    QTemporaryDir m_dir;
};

QTEST_GUILESS_MAIN(TestSettingsStore)
#include "test_settingsstore.moc"
