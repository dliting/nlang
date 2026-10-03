/*--- test_searchpathargs.cpp - SearchPathArgs pure-function tests ---*/
#include "../../../src/tools/nide/SearchPathArgs.h"

#include <QDir>
#include <QtTest>

using namespace nlang;

class TestSearchPathArgs : public QObject {
    Q_OBJECT

private slots:
    void testProjectThenGlobalOrder() {
        QStringList args = buildImportArgs(
            {"D:/proj/lib"}, "D:/proj", {"D:/global/lib"});
        QCOMPARE(args, QStringList({"-I", "D:/proj/lib",
                                    "-I", "D:/global/lib"}));
    }

    void testProjectRelativeResolvesAgainstProjectDir() {
        QStringList dirs = buildSearchDirs(
            {"libs/acme"}, "D:/proj", {});
        QCOMPARE(dirs, QStringList({"D:/proj/libs/acme"}));
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
            {"D:/shared"}, "D:/proj", {"D:/shared"});
        QCOMPARE(dirs, QStringList({"D:/shared"}));
    }

    void testEmptyAndWhitespaceEntriesDropped() {
        QStringList dirs = buildSearchDirs(
            {"", "  "}, "D:/proj", {"D:/ok", ""});
        QCOMPARE(dirs, QStringList({"D:/ok"}));
    }

    void testAppendImportArgsInterleaves() {
        QCOMPARE(appendImportArgs({"a", "b"}),
                 QStringList({"-I", "a", "-I", "b"}));
    }

    void testStandaloneAppendsSourceDirAfterGlobals() {
        //Globals are user-configured: they keep precedence over the
        //incidental same-dir library.
        QStringList dirs = standaloneSearchDirs(
            "D:/src/use_lib.n", {"D:/global/lib"});
        QCOMPARE(dirs, QStringList({"D:/global/lib", "D:/src"}));
    }

    void testStandaloneDedupsGlobalEqualToSourceDir() {
        QStringList dirs = standaloneSearchDirs(
            "D:/src/use_lib.n", {"D:/src"});
        QCOMPARE(dirs, QStringList({"D:/src"}));
    }

    void testStandaloneWithoutGlobalsIsSourceDirOnly() {
        QStringList dirs = standaloneSearchDirs("D:/src/use_lib.n", {});
        QCOMPARE(dirs, QStringList({"D:/src"}));
    }

    void testProjectAppendsProjectDirAfterGlobals() {
        //The project dir is the compile side's implicit base dir; it
        //rides last so configured dirs keep precedence.
        QStringList dirs = projectSearchDirs(
            {"D:/proj/lib"}, "D:/proj", {"D:/global/lib"});
        QCOMPARE(dirs, QStringList(
            {"D:/proj/lib", "D:/global/lib", "D:/proj"}));
    }

    void testProjectDedupsProjectDirEqualToEntry() {
        //An import path that resolves to the project dir itself must
        //not duplicate the appended base entry.
        QStringList dirs = projectSearchDirs({ "." }, "D:/proj", {});
        QCOMPARE(dirs, QStringList({"D:/proj"}));
    }
};

//GUILESS: the nide support lib pulls in Qt5::Gui; a plain QTEST_MAIN
//would upgrade to QGuiApplication and demand a platform plugin.
QTEST_GUILESS_MAIN(TestSearchPathArgs)
#include "test_searchpathargs.moc"
