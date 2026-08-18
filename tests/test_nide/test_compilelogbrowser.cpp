/*--- test_compilelogbrowser.cpp - CompileLogBrowser unit tests ---*/
#include "CompileLogBrowser.h"
#include "ProjectModel.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace nlang;

namespace {

const char* kErrorLine = "E:/src/main.n(line 3, char 7): Error: syntax error";

} // namespace

class TestCompileLogBrowser : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        //QSignalSpy captures signal args in QVariants. The lookup name
        //is the parameter type as moc recorded it -- the bare class
        //name, without the namespace.
        qRegisterMetaType<CompileLogItemInfo>("CompileLogItemInfo");
    }

    void testParseValidLogLine() {
        CompileLogItemInfo info;
        QVERIFY(CompileLogBrowser::parseLogLine(kErrorLine, info));
        QCOMPARE(info.filePath, QString("E:/src/main.n"));
        QCOMPARE(info.line, static_cast<size_t>(3));
        QCOMPARE(info.column, static_cast<size_t>(7));
        QCOMPARE(info.message, QString("Error: syntax error"));
    }

    void testParseRejectsPlainLine() {
        CompileLogItemInfo info;
        QVERIFY(!CompileLogBrowser::parseLogLine("Compilation failed.", info));
        QVERIFY(!CompileLogBrowser::parseLogLine("", info));
        QVERIFY(!CompileLogBrowser::parseLogLine("main.n(line 3): Error", info));
    }

    void testDoubleClickEmitsParsedInfo() {
        CompileLogBrowser browser;
        browser.setPlainText(kErrorLine);

        QSignalSpy spy(&browser, &CompileLogBrowser::lineSelected);
        QTextCursor cursor = browser.textCursor();
        cursor.movePosition(QTextCursor::Start);
        browser.setTextCursor(cursor);
        QTest::mouseDClick(browser.viewport(), Qt::LeftButton,
                           Qt::NoModifier, browser.cursorRect(cursor).center());

        QCOMPARE(spy.count(), 1);
        const CompileLogItemInfo info =
            spy.at(0).at(0).value<CompileLogItemInfo>();
        QCOMPARE(info.filePath, QString("E:/src/main.n"));
        QCOMPARE(info.line, static_cast<size_t>(3));
        QCOMPARE(info.column, static_cast<size_t>(7));
        QCOMPARE(info.message, QString("Error: syntax error"));
    }

    void testDoubleClickResolvesRelativePathViaProject() {
        QTemporaryDir dir;
        ProjectNode project("App", dir.path());
        CompileLogBrowser browser;
        browser.setProject(&project);
        browser.setPlainText("main.n(line 1, char 2): Error: oops");

        QSignalSpy spy(&browser, &CompileLogBrowser::lineSelected);
        QTextCursor cursor = browser.textCursor();
        cursor.movePosition(QTextCursor::Start);
        browser.setTextCursor(cursor);
        QTest::mouseDClick(browser.viewport(), Qt::LeftButton,
                           Qt::NoModifier, browser.cursorRect(cursor).center());

        QCOMPARE(spy.count(), 1);
        const CompileLogItemInfo info =
            spy.at(0).at(0).value<CompileLogItemInfo>();
        QCOMPARE(info.filePath, project.absolutePathOf("main.n"));
        QCOMPARE(info.line, static_cast<size_t>(1));
    }

    void testDoubleClickPlainLineEmitsWholeLine() {
        CompileLogBrowser browser;
        browser.setPlainText("Compilation failed.");

        QSignalSpy spy(&browser, &CompileLogBrowser::lineSelected);
        QTextCursor cursor = browser.textCursor();
        cursor.movePosition(QTextCursor::Start);
        browser.setTextCursor(cursor);
        QTest::mouseDClick(browser.viewport(), Qt::LeftButton,
                           Qt::NoModifier, browser.cursorRect(cursor).center());

        //A line without the <file>(line N, char N): shape carries no
        //location, just the whole text.
        QCOMPARE(spy.count(), 1);
        const CompileLogItemInfo info =
            spy.at(0).at(0).value<CompileLogItemInfo>();
        QCOMPARE(info.filePath, QString());
        QCOMPARE(info.line, static_cast<size_t>(0));
        QCOMPARE(info.column, static_cast<size_t>(0));
        QCOMPARE(info.message, QString("Compilation failed."));
    }

    void testDoubleClickKeepsAbsolutePathWithProject() {
        QTemporaryDir dir;
        ProjectNode project("App", dir.path());
        CompileLogBrowser browser;
        browser.setProject(&project);
        browser.setPlainText(kErrorLine);

        QSignalSpy spy(&browser, &CompileLogBrowser::lineSelected);
        QTextCursor cursor = browser.textCursor();
        cursor.movePosition(QTextCursor::Start);
        browser.setTextCursor(cursor);
        QTest::mouseDClick(browser.viewport(), Qt::LeftButton,
                           Qt::NoModifier, browser.cursorRect(cursor).center());

        //An attached project must resolve only relative paths; the
        //absolute diagnostic path passes through unchanged.
        QCOMPARE(spy.count(), 1);
        const CompileLogItemInfo info =
            spy.at(0).at(0).value<CompileLogItemInfo>();
        QCOMPARE(info.filePath, QString("E:/src/main.n"));
    }

    void testDoubleClickOnSecondLineParsesClickedLine() {
        CompileLogBrowser browser;
        //Geometry needs two things an unshown widget does not give for
        //free: a viewport tall enough to hold every line, and a laid-out
        //document (the layout is lazy -- until adjustSize() every block
        //rect is (0,0) and any click maps to line 1).
        browser.resize(400, 120);
        browser.setPlainText(
            "E:/src/first.n(line 1, char 1): Error: first\n"
            "E:/src/second.n(line 2, char 5): Error: second\n"
            "E:/src/third.n(line 3, char 9): Error: third");
        browser.document()->adjustSize();

        //Click the middle line by its own rect: the click position (not
        //a pre-seeded cursor) must drive which line is parsed.
        QSignalSpy spy(&browser, &CompileLogBrowser::lineSelected);
        QTextCursor secondLine = browser.textCursor();
        secondLine.movePosition(QTextCursor::Start);
        secondLine.movePosition(QTextCursor::NextBlock);
        QTest::mouseDClick(browser.viewport(), Qt::LeftButton,
                           Qt::NoModifier,
                           browser.cursorRect(secondLine).center());

        QCOMPARE(spy.count(), 1);
        const CompileLogItemInfo info =
            spy.at(0).at(0).value<CompileLogItemInfo>();
        QCOMPARE(info.filePath, QString("E:/src/second.n"));
        QCOMPARE(info.line, static_cast<size_t>(2));
        QCOMPARE(info.column, static_cast<size_t>(5));
        QCOMPARE(info.message, QString("Error: second"));
    }

    void testDoubleClickOnBlankLineEmitsNothing() {
        CompileLogBrowser browser;
        //Same geometry prerequisites as the multi-line test above.
        browser.resize(400, 120);
        browser.setPlainText("E:/src/a.n(line 1, char 1): Error: a\n");
        browser.document()->adjustSize();

        QSignalSpy spy(&browser, &CompileLogBrowser::lineSelected);
        QTextCursor blankLine = browser.textCursor();
        blankLine.movePosition(QTextCursor::Start);
        blankLine.movePosition(QTextCursor::NextBlock);
        QTest::mouseDClick(browser.viewport(), Qt::LeftButton,
                           Qt::NoModifier,
                           browser.cursorRect(blankLine).center());

        QCOMPARE(spy.count(), 0);
    }

    void testRightDoubleClickEmitsNothing() {
        //QtTest prints a "MouseDClick not accepted" WARNING here: the
        //base class ignores non-left double-clicks, by design. The
        //override still runs -- expect the warning, the test is fine.
        CompileLogBrowser browser;
        //Suppress the default context menu -- a modal exec() would hang
        //the test (the QPA harness trap).
        browser.setContextMenuPolicy(Qt::NoContextMenu);
        browser.setPlainText(kErrorLine);

        QSignalSpy spy(&browser, &CompileLogBrowser::lineSelected);
        QTextCursor cursor = browser.textCursor();
        cursor.movePosition(QTextCursor::Start);
        browser.setTextCursor(cursor);
        QTest::mouseDClick(browser.viewport(), Qt::RightButton,
                           Qt::NoModifier, browser.cursorRect(cursor).center());

        QCOMPARE(spy.count(), 0);
    }
};

QTEST_MAIN(TestCompileLogBrowser)
#include "test_compilelogbrowser.moc"
