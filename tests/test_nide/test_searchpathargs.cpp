/*--- test_searchpathargs.cpp - SearchPathArgs pure-function tests ---*/
#include "../../../src/tools/nide/SearchPathArgs.h"

#include <QDir>
#include <QtTest>

using namespace nlang;

namespace {

//The pure functions under test branch on QDir::isAbsolutePath, so the
//fixtures must BE absolute on the platform they run on: drive-letter
//spelling on Windows, root-anchored paths elsewhere. Every expected
//value below is built from the same constants, so the join shapes the
//assertions pin stay identical across platforms.
#ifdef _WIN32
const QString kProj = "D:/proj";
const QString kGlobal = "D:/global";
const QString kShared = "D:/shared";
const QString kSrc = "D:/src";
#else
const QString kProj = "/proj";
const QString kGlobal = "/global";
const QString kShared = "/shared";
const QString kSrc = "/src";
#endif

} // namespace

class TestSearchPathArgs : public QObject {
    Q_OBJECT

private slots:
    void testProjectThenGlobalOrder() {
        QStringList args = buildImportArgs(
            {kProj + "/lib"}, kProj, {kGlobal + "/lib"});
        QCOMPARE(args, QStringList({"-I", kProj + "/lib",
                                    "-I", kGlobal + "/lib"}));
    }

    void testProjectRelativeResolvesAgainstProjectDir() {
        QStringList dirs = buildSearchDirs(
            {"libs/acme"}, kProj, {});
        QCOMPARE(dirs, QStringList({kProj + "/libs/acme"}));
    }

    void testGlobalRelativeResolvesAgainstHome() {
        const QString home = globalPathBase();
        QStringList dirs = buildSearchDirs({}, QString(), {"vendor/x"});
        QCOMPARE(dirs,
                 QStringList({QDir(home).absoluteFilePath("vendor/x")}));
    }

    void testDuplicatesAcrossLayersDropped() {
        //The same dir in project and global keeps only the first.
        QStringList dirs = buildSearchDirs(
            {kShared}, kProj, {kShared});
        QCOMPARE(dirs, QStringList({kShared}));
    }

    void testEmptyAndWhitespaceEntriesDropped() {
        QStringList dirs = buildSearchDirs(
            {"", "  "}, kProj, {kGlobal + "/ok", ""});
        QCOMPARE(dirs, QStringList({kGlobal + "/ok"}));
    }

    void testAppendImportArgsInterleaves() {
        QCOMPARE(appendImportArgs({"a", "b"}),
                 QStringList({"-I", "a", "-I", "b"}));
    }

    void testStandaloneAppendsSourceDirAfterGlobals() {
        //Globals are user-configured: they keep precedence over the
        //incidental same-dir library.
        QStringList dirs = standaloneSearchDirs(
            kSrc + "/use_lib.n", {kGlobal + "/lib"});
        QCOMPARE(dirs, QStringList({kGlobal + "/lib", kSrc}));
    }

    void testStandaloneDedupsGlobalEqualToSourceDir() {
        QStringList dirs = standaloneSearchDirs(
            kSrc + "/use_lib.n", {kSrc});
        QCOMPARE(dirs, QStringList({kSrc}));
    }

    void testStandaloneWithoutGlobalsIsSourceDirOnly() {
        QStringList dirs = standaloneSearchDirs(kSrc + "/use_lib.n", {});
        QCOMPARE(dirs, QStringList({kSrc}));
    }

    void testProjectAppendsProjectDirAfterGlobals() {
        //The project dir is the compile side's implicit base dir; it
        //rides last so configured dirs keep precedence.
        QStringList dirs = projectSearchDirs(
            {kProj + "/lib"}, kProj, {kGlobal + "/lib"});
        QCOMPARE(dirs, QStringList(
            {kProj + "/lib", kGlobal + "/lib", kProj}));
    }

    void testProjectDedupsProjectDirEqualToEntry() {
        //An import path that resolves to the project dir itself must
        //not duplicate the appended base entry.
        QStringList dirs = projectSearchDirs({ "." }, kProj, {});
        QCOMPARE(dirs, QStringList({kProj}));
    }
};

//GUILESS: the nide support lib pulls in Qt5::Gui; a plain QTEST_MAIN
//would upgrade to QGuiApplication and demand a platform plugin.
QTEST_GUILESS_MAIN(TestSearchPathArgs)
#include "test_searchpathargs.moc"
