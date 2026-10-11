// --- RunErrorBrowser unit tests: run-output parsing + navigation ---
#include "RunErrorBrowser.h"

#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

using nlang::RunErrorBrowser;
using nlang::RunErrorItemInfo;

class TestRunErrorBrowser : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void parseRunLineShapes();
    void setRunOutputPopulatesAndNavigates();
    void setRunOutputAcceptsCrlfLines();
};

void TestRunErrorBrowser::initTestCase() {
    //QSignalSpy must be able to copy the payload out of the signal.
    qRegisterMetaType<RunErrorItemInfo>("RunErrorItemInfo");
}

void TestRunErrorBrowser::parseRunLineShapes() {
    RunErrorItemInfo info;
    QVERIFY(RunErrorBrowser::parseRunLine(
        "Runtime error: NLang VM: boom", info));
    QCOMPARE(info.line, 0);
    QVERIFY(info.filePath.isEmpty());

    QVERIFY(RunErrorBrowser::parseRunLine(
        "  at main (E:/x/app.n:7)", info));
    QCOMPARE(info.filePath, QStringLiteral("E:/x/app.n"));
    QCOMPARE(info.line, 7);

    QVERIFY(RunErrorBrowser::parseRunLine(
        "  at main (E:/x/app.n:?)", info));
    QCOMPARE(info.line, 0);   //the `?` form: no site

    QVERIFY(!RunErrorBrowser::parseRunLine("hello world", info));
    QVERIFY(!RunErrorBrowser::parseRunLine("Program exited with code 42.", info));
    QVERIFY(!RunErrorBrowser::parseRunLine("", info));
}

void TestRunErrorBrowser::setRunOutputPopulatesAndNavigates() {
    RunErrorBrowser browser;
    //Geometry needs two things an unshown widget does not give for
    //free: a viewport tall enough to hold every line, and a laid-out
    //document (the layout is lazy -- until adjustSize() every block
    //rect is (0,0) and any click maps to line 1).
    browser.resize(400, 120);
    QSignalSpy spy(&browser, &RunErrorBrowser::itemSelected);
    browser.setRunOutput(QStringLiteral(
        "some output\n"
        "Runtime error: boom\n"
        "Backtrace:\n"
        "  at helper (E:/x/lib.n:3)\n"
        "  at main (E:/x/main.n:9)\n"
        "Program exited with code 1.\n"));
    //Kept rows = 3 (message + two frames); "some output"/"Backtrace:"/
    //"Program exited…" parse to nothing. QTextEdit::append fills the
    //initial block with the FIRST append and adds no trailing empty
    //block, so 3 appends → blockCount 3 (blocks map 1:1 onto m_items,
    //which is exactly what emitItemUnderCursor relies on).
    QTextDocument* doc = browser.document();
    QCOMPARE(doc->blockCount(), 3);
    doc->adjustSize();

    //Navigate block 0 = the message row: set the cursor onto the
    //block, then double-click the VIEWPORT at the cursor's rect —
    //cursorRect is in viewport coordinates.
    QTextCursor cursor(doc->findBlockByNumber(0));
    browser.setTextCursor(cursor);
    QTest::mouseDClick(browser.viewport(), Qt::LeftButton,
                       Qt::NoModifier, browser.cursorRect(cursor).center());
    QCOMPARE(spy.count(), 1);
    RunErrorItemInfo fired = spy.at(0).at(0).value<RunErrorItemInfo>();
    QCOMPARE(fired.message, QStringLiteral("Runtime error: boom"));
    QVERIFY(fired.filePath.isEmpty());   //message row: no site

    //Navigate block 1 = the first backtrace frame (helper).
    cursor = QTextCursor(doc->findBlockByNumber(1));
    browser.setTextCursor(cursor);
    QTest::mouseDClick(browser.viewport(), Qt::LeftButton,
                       Qt::NoModifier, browser.cursorRect(cursor).center());
    QCOMPARE(spy.count(), 2);
    fired = spy.at(1).at(0).value<RunErrorItemInfo>();
    QCOMPARE(fired.filePath, QStringLiteral("E:/x/lib.n"));
    QCOMPARE(fired.line, 3);

    //Empty text clears back to the single initial block.
    browser.setRunOutput(QString());
    QCOMPARE(browser.document()->blockCount(), 1);
}

void TestRunErrorBrowser::setRunOutputAcceptsCrlfLines() {
    //ConPTY delivers every line break as CRLF: after the '\n' split
    //each line still ends with '\r', which the '$'-anchored patterns
    //must not choke on (the real tail comes from raw PTY bytes).
    RunErrorBrowser browser;
    browser.resize(400, 120);
    QSignalSpy spy(&browser, &RunErrorBrowser::itemSelected);
    browser.setRunOutput(QStringLiteral(
        "Runtime error: boom\r\n"
        "Backtrace:\r\n"
        "  at main (E:/x/main.n:9)\r\n"));
    QTextDocument* doc = browser.document();
    QCOMPARE(doc->blockCount(), 2);   //message row + one frame row
    doc->adjustSize();

    //Block 0 = the message row, carrying no trailing '\r'.
    QTextCursor cursor(doc->findBlockByNumber(0));
    browser.setTextCursor(cursor);
    QTest::mouseDClick(browser.viewport(), Qt::LeftButton,
                       Qt::NoModifier, browser.cursorRect(cursor).center());
    QCOMPARE(spy.count(), 1);
    RunErrorItemInfo fired = spy.at(0).at(0).value<RunErrorItemInfo>();
    QCOMPARE(fired.message, QStringLiteral("Runtime error: boom"));
    QVERIFY(fired.filePath.isEmpty());

    //Block 1 = the frame row, navigable to its site.
    cursor = QTextCursor(doc->findBlockByNumber(1));
    browser.setTextCursor(cursor);
    QTest::mouseDClick(browser.viewport(), Qt::LeftButton,
                       Qt::NoModifier, browser.cursorRect(cursor).center());
    QCOMPARE(spy.count(), 2);
    fired = spy.at(1).at(0).value<RunErrorItemInfo>();
    QCOMPARE(fired.filePath, QStringLiteral("E:/x/main.n"));
    QCOMPARE(fired.line, 9);
}

QTEST_MAIN(TestRunErrorBrowser)
#include "test_runerrorbrowser.moc"
