// Embedded terminal tests: TerminalEmulator over the vendored libvterm
// (cell readout, damage, scrollback, key encoding), the TerminalWidget
// render/input model, and PtyProcess with real child processes.

#include <QtTest>

#include "terminal/TerminalEmulator.h"
#include "terminal/TerminalWidget.h"
#include "terminal/LineEditor.h"
#include "terminal/PtyProcess.h"

#include <QApplication>
#include <QInputMethodEvent>
#include <QSignalSpy>
#include <cstdio>
#include <string>

using namespace nlang::terminal;

namespace {
//Byte sink that captures keyboard-reply bytes for assertions.
struct CaptureSink {
    std::string bytes;
    TerminalEmulator::ByteSink sink() {
        return [this](const std::string& chunk) { bytes += chunk; };
    }
};
} // namespace

class TestTerminal : public QObject {
    Q_OBJECT

private slots:
    //--- emulator ---
    void TestPlainTextCells();
    void TestSgrColorsAndAttributes();
    void TestCursorMovement();
    void TestScrollIntoScrollback();
    void TestChineseWideChars();
    void TestResetClears();
    void TestResetHomesCursor();
    void TestResize();
    void TestResizeTracksCursor();
    void TestSendCharEncodes();
    void TestSendKeyEncodes();

    //--- widget render ---
    void TestFeedAndScreenText();
    void TestResetTerminal();
    void TestFeedUtf8NormalizesNewline();
    void TestColumnsRowsFromSize();
    void TestSelectionAndCopy();
    void TestBlankCellReadsAsSpace();
    void TestWideCharPaintsBothCells();
    void TestWideCharPaintsAtOffset();
    void TestWideCharPendingLinePaints();
    void TestWideCharCursorBlockSpansCell();
    void TestLineModePromptPainted();
    void TestLineModePromptEcho();

    //--- input modes / IME ---
    void TestLineEditorScalars();
    void TestCharacterModeKeyPassthrough();
    void TestEnterEncodesCr();
    void TestBackspaceDel();
    void TestArrowCsi();
    void TestCtrlCWithoutSelection();
    void TestLineModeEchoAndCommit();
    void TestLineModeHistory();
    void TestLineModeBackspaceEditing();
    void TestEmptyLineEchoOnly();
    void TestPaste();
    void TestImeCommit();

    //--- pty (real child processes) ---
    void TestPtyEchoHello();
    void TestPtyExitCode();
    void TestPtyKill();
    void TestPtyWriteRoundTrip();
};

//--- emulator ----------------------------------------------------------------

void TestTerminal::TestPlainTextCells() {
    TerminalEmulator emulator;
    emulator.Feed("hello");
    QCOMPARE(emulator.CellAt(0, 0).chars[0], uint32_t('h'));
    QCOMPARE(emulator.LineText(0), std::string("hello"));
    QVERIFY(emulator.ScreenText().find("hello") != std::string::npos);
}

void TestTerminal::TestSgrColorsAndAttributes() {
    TerminalEmulator emulator;
    emulator.Feed("\x1b[31mR\x1b[0m\x1b[1;4;7mB\x1b[0mN");
    //SGR 31 = palette index 1 (Campbell red 0xC50F1F), converted to RGB.
    QCOMPARE(emulator.CellAt(0, 0).foreground, 0xC50F1Fu);
    const TermCell bold = emulator.CellAt(0, 1);
    QVERIFY(bold.bold);
    QCOMPARE(bold.underline, int(VTERM_UNDERLINE_SINGLE));
    QVERIFY(bold.reverse);
    //After SGR 0 everything is back to the palette defaults.
    QCOMPARE(emulator.CellAt(0, 2).foreground, 0xCCCCCCu);
    QVERIFY(!emulator.CellAt(0, 2).bold);
}

void TestTerminal::TestCursorMovement() {
    TerminalEmulator emulator;
    emulator.Feed("ab");
    QCOMPARE(emulator.cursorColumn(), 2);
    emulator.Feed("\x1b[2D");      // cursor back 2
    QCOMPARE(emulator.cursorColumn(), 0);
    emulator.Feed("\x1b[3;5H");    // CUP to row 3, column 5
    QCOMPARE(emulator.cursorRow(), 2);
    QCOMPARE(emulator.cursorColumn(), 4);
}

void TestTerminal::TestScrollIntoScrollback() {
    TerminalEmulator emulator;     // 24 rows
    for (int i = 0; i < 30; ++i) {
        char line[16];
        //Real child output is CRLF: LF alone only moves down (the column
        //staircases into the right margin and an extra wrap-scroll).
        std::snprintf(line, sizeof(line), "L%02d\r\n", i);
        emulator.Feed(line);
    }
    //Cursor starts at row 0: the first 23 linefeeds only move down;
    //newlines 24..30 (including L29's trailing one) each scroll a line
    //off — L00..L06 land in the scrollback (7 lines), L07..L29 stay on
    //screen. Count verified against the real feed behaviour (the plan's
    //6 missed the final trailing newline's scroll).
    QCOMPARE(int(emulator.scrollback().size()), 7);
    QVERIFY(emulator.ScreenText().find("L29") != std::string::npos);
    QVERIFY(emulator.ScreenText().find("L00") == std::string::npos);
}

void TestTerminal::TestChineseWideChars() {
    TerminalEmulator emulator;
    emulator.Feed("中x");
    const TermCell wide = emulator.CellAt(0, 0);
    QCOMPARE(wide.chars[0], 0x4E2Du);
    QCOMPARE(wide.width, 2);
    //The narrow follower lands two columns right; the skip iteration in
    //LineText never reads the wide char's right half.
    QCOMPARE(emulator.CellAt(0, 2).chars[0], uint32_t('x'));
    QCOMPARE(emulator.LineText(0), std::string("中x"));
}

void TestTerminal::TestResetClears() {
    TerminalEmulator emulator;
    emulator.Feed("junk\nmore");
    for (int i = 0; i < 30; ++i)
        emulator.Feed("scroll\n");
    QVERIFY(emulator.scrollback().size() > 0);
    emulator.Reset();
    QCOMPARE(emulator.ScreenText(), std::string(""));
    QCOMPARE(emulator.scrollback().size(), size_t(0));
    //The palette survives reset (Reset re-injects it).
    emulator.Feed("\x1b[31mR");
    QCOMPARE(emulator.CellAt(0, 0).foreground, 0xC50F1Fu);
}

//vterm_state_reset homes the real cursor to (0,0) WITHOUT a movecursor
//callback (only the CSI 'c' dispatcher emits one, and vterm_screen_reset
//bypasses it), so a callback-mirrored cursor stays at the previous
//session's last position and the line-mode prompt drew mid-screen over
//the freshly blanked grid. The tracked cursor must follow the reset.
void TestTerminal::TestResetHomesCursor() {
    TerminalEmulator emulator;
    emulator.Feed("abc\r\ndef\r\nxy");
    QCOMPARE(emulator.cursorRow(), 2);
    QCOMPARE(emulator.cursorColumn(), 2);
    emulator.Reset();
    QCOMPARE(emulator.cursorRow(), 0);
    QCOMPARE(emulator.cursorColumn(), 0);
}

void TestTerminal::TestResize() {
    TerminalEmulator emulator;
    emulator.Resize(100, 40);
    QCOMPARE(emulator.columns(), 100);
    QCOMPARE(emulator.rows(), 40);
    emulator.Feed("\x1b[40;100H" "x");   // bottom-right corner
    QCOMPARE(emulator.CellAt(39, 99).chars[0], uint32_t('x'));
}

//Growing the grid backfills from the scrollback and shifts the cursor
//down one row per restored line (on_resize ends in updatecursor, so the
//move does arrive as a callback). Pins that the reported cursor lands
//on the reflowed position — bottom row, one past the restored content
//— which the line-mode prompt position rides on.
void TestTerminal::TestResizeTracksCursor() {
    TerminalEmulator emulator;     // 24 rows
    for (int i = 0; i < 30; ++i) {
        char line[16];
        std::snprintf(line, sizeof(line), "L%02d\r\n", i);
        emulator.Feed(line);
    }
    QCOMPARE(emulator.cursorRow(), 23);   //bottom row after the scrolls
    emulator.Resize(80, 30);   //grow: 6 scrollback lines return
    QCOMPARE(int(emulator.scrollback().size()), 1);   //L00 stays out
    QCOMPARE(emulator.cursorRow(), 29);   //pushed down by 6 restorations
    QCOMPARE(emulator.cursorColumn(), 0);
    QVERIFY(emulator.ScreenText().find("L01") != std::string::npos);
    QVERIFY(emulator.ScreenText().find("L00") == std::string::npos);
}

void TestTerminal::TestSendCharEncodes() {
    TerminalEmulator emulator;
    CaptureSink sink;
    emulator.SetByteSink(sink.sink());
    emulator.SendChar(uint32_t('a'), VTERM_MOD_NONE);
    emulator.SendChar(0x03, VTERM_MOD_NONE);   // C0 passthrough (Ctrl+C)
    QCOMPARE(sink.bytes, std::string("a\x03"));
}

void TestTerminal::TestSendKeyEncodes() {
    TerminalEmulator emulator;
    CaptureSink sink;
    emulator.SetByteSink(sink.sink());
    emulator.SendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
    emulator.SendKey(VTERM_KEY_BACKSPACE, VTERM_MOD_NONE);
    emulator.SendKey(VTERM_KEY_UP, VTERM_MOD_NONE);
    QCOMPARE(sink.bytes, std::string("\r\x7f\x1b[A"));
    sink.bytes.clear();
    emulator.Feed("\x1b[?1h");    // DECCKM: application cursor mode
    emulator.SendKey(VTERM_KEY_UP, VTERM_MOD_NONE);
    QCOMPARE(sink.bytes, std::string("\x1bOA"));
}

//--- widget render -----------------------------------------------------------

void TestTerminal::TestFeedAndScreenText() {
    TerminalWidget widget;
    widget.feedBytes("hello world");
    QVERIFY(widget.screenText().find("hello world") != std::string::npos);
}

void TestTerminal::TestResetTerminal() {
    TerminalWidget widget;
    widget.feedBytes("junk\nmore junk");
    for (int i = 0; i < 30; ++i)
        widget.feedBytes("scroll\n");
    //"junk" has already scrolled into the scrollback (30 more lines
    //followed it); the screen itself shows the "scroll" fill lines.
    QVERIFY(widget.screenText().find("scroll") != std::string::npos);
    widget.resetTerminal();
    QCOMPARE(widget.screenText(), std::string(""));
}

//feedUtf8 carries cooked text (debug-channel output and lifecycle
//diagnostics), where a lone '\n' means "this line is done" — but a
//terminal newline is CRLF, and libvterm's bare LF only moves down, so
//the column survives every newline and program output staircases to
//the right (the wire even splits io.print into a text half plus a
//literal-\n half). feedUtf8 must normalize every bare LF to CRLF, and
//an explicit CRLF passes through as ONE line break.
void TestTerminal::TestFeedUtf8NormalizesNewline() {
    TerminalWidget widget;
    widget.feedUtf8("line one");
    widget.feedUtf8("\n");          //the wire's newline half
    widget.feedUtf8("line two");
    widget.feedUtf8("\n");
    QCOMPARE(widget.screenText(), std::string("line one\nline two"));

    TerminalWidget embedded;
    embedded.feedUtf8("a\nb\r\nc");
    QCOMPARE(embedded.screenText(), std::string("a\nb\nc"));
}

void TestTerminal::TestColumnsRowsFromSize() {
    TerminalWidget widget;
    //A hidden widget's resize() defers QResizeEvent until show (Qt
    //documented semantics; the project has been bitten by lazy layout
    //before), and the relayout rides on resizeEvent — so expose the
    //window first; after that, resize delivers the event synchronously.
    widget.show();
    QTest::qWaitForWindowExposed(&widget);
    widget.resize(400, 300);
    const int smallColumns = widget.columns();
    QVERIFY(smallColumns >= 20);   //kMinColumns floor
    widget.resize(1200, 800);
    QVERIFY(widget.columns() > smallColumns);   //grid grows with the size
}

void TestTerminal::TestSelectionAndCopy() {
    TerminalWidget widget;
    widget.feedBytes("hello world");
    //Press at cell (0,0), release at cell (4,0): the selection spans the
    //first five cells ("hell" plus one). QTest sends the events straight
    //to the widget, so no window exposure is needed.
    QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier,
                      widget.cellCenter(0, 0));
    QTest::mouseRelease(&widget, Qt::LeftButton, Qt::NoModifier,
                        widget.cellCenter(4, 0));
    QVERIFY(widget.hasSelection());
    //The selection spans cells 0..4 — exactly "hello". Asserting our
    //own extraction is stronger than the clipboard text: the system
    //clipboard is Qt/OS territory and this machine's clipboard manager
    //keeps it locked (OleSetClipboard CLIPBRD_E_CANT_OPEN), which made
    //the read-back assertion fail for reasons outside this code.
    QCOMPARE(widget.selectedText(), QString("hello"));
    widget.copySelection();   //smoke: must not assert/crash
}

//conhost steps over a gap in a line with CUF (ESC [1C) instead of
//writing a space — every interactive prompt under ConPTY ("Name: > ")
//arrives this way. The blank cell it leaves behind is screen space, so
//both read-back paths must synthesize a space; before that, a line lost
//its interior blanks ("Name: >" read back as "Name:>").
void TestTerminal::TestBlankCellReadsAsSpace() {
    TerminalEmulator emulator;
    emulator.Feed("Name:\x1b[1C>");
    QCOMPARE(emulator.LineText(0), std::string("Name: >"));
    QCOMPARE(emulator.CellAt(0, 5).charCount, 0);   //the gap cell itself

    //One space per blank column, and a trailing gap trims like any
    //other trailing blank.
    TerminalEmulator gapEmulator;
    gapEmulator.Feed("a\x1b[2Cb");
    QCOMPARE(gapEmulator.LineText(0), std::string("a  b"));
    TerminalEmulator trailingEmulator;
    trailingEmulator.Feed("abc\x1b[2C");
    QCOMPARE(trailingEmulator.LineText(0), std::string("abc"));

    //A wide glyph half-erased by ECH: the erased left column is blank
    //but still spans the pair, so both of its columns read back blank.
    TerminalEmulator wideEmulator;
    wideEmulator.Feed("\xe4\xb8\xadZ\x1b[3D\x1b[1X");
    QCOMPARE(wideEmulator.CellAt(0, 0).charCount, 0);
    QCOMPARE(wideEmulator.LineText(0), std::string("  Z"));

    TerminalWidget widget;
    widget.feedBytes("Name:\x1b[1C>");
    QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier,
                      widget.cellCenter(0, 0));
    QTest::mouseRelease(&widget, Qt::LeftButton, Qt::NoModifier,
                        widget.cellCenter(6, 0));
    QCOMPARE(widget.selectedText(), QString("Name: >"));
}

//Counts non-background pixels in a small window around a cell centre:
//the ink probe for the wide-glyph tests (the fixed background makes an
//exact compare reliable; antialiased glyph edges count as ink).
static int InkAroundCentre(const QImage& shot, const TerminalWidget& widget,
                           int column, int row) {
    const QPoint centre = widget.cellCenter(column, row);
    const QColor background(0x0C, 0x0C, 0x0C);   //kDefaultBackground
    const int halfBandW = widget.cellWidth() / 4;
    const int halfBandH = widget.cellHeight() / 4;
    int ink = 0;
    for (int x = centre.x() - halfBandW; x <= centre.x() + halfBandW; ++x)
        for (int y = centre.y() - halfBandH; y <= centre.y() + halfBandH; ++y)
            if (shot.pixelColor(x, y) != background)
                ++ink;
    return ink;
}

void TestTerminal::TestWideCharPaintsBothCells() {
    TerminalWidget widget;
    widget.show();
    QTest::qWaitForWindowExposed(&widget);
    widget.feedBytes("\xE4\xB8\xAD");   // 中: one glyph spanning two cells
    const QImage shot = widget.grab().toImage();
    //The glyph's ink must reach the SECOND cell (a single-cell clip
    //blanked the right half — the wide-char display bug).
    QVERIFY2(InkAroundCentre(shot, widget, 1, 0) > 3,
             "the wide glyph must paint into its second grid cell");
}

void TestTerminal::TestWideCharPaintsAtOffset() {
    TerminalWidget widget;
    widget.show();
    QTest::qWaitForWindowExposed(&widget);
    widget.feedBytes("a\xE4\xB8\xAD");   //a then 中 (columns 1-2)
    const QImage shot = widget.grab().toImage();
    //The wide glyph occupies columns 1-2: ink must sit in column 1. (A
    //rect that multiplies the column by the SPAN would shift the glyph
    //to columns 2-3 and leave column 1 blank.)
    QVERIFY2(InkAroundCentre(shot, widget, 1, 0) > 3,
             "a wide glyph after a narrow one must paint at its own cell");
}

void TestTerminal::TestWideCharPendingLinePaints() {
    TerminalWidget widget;
    widget.setMode(TerminalWidget::Mode::Line);
    widget.show();
    QTest::qWaitForWindowExposed(&widget);
    widget.pasteText(QStringLiteral("中"));   //into the pending edit line
    QCOMPARE(widget.pendingLine(), QStringLiteral("中"));
    const QImage shot = widget.grab().toImage();
    //The line prompt occupies columns 0-1, so the wide glyph starts at
    //column 2; its second cell is column 3.
    QVERIFY2(InkAroundCentre(shot, widget, 3, 0) > 3,
             "the wide pending glyph must paint into its second cell");
}

void TestTerminal::TestWideCharCursorBlockSpansCell() {
    TerminalWidget widget;
    widget.setMode(TerminalWidget::Mode::Character);   //block cursor mode
    widget.show();
    QTest::qWaitForWindowExposed(&widget);
    widget.feedBytes("\xE4\xB8\xAD");   // 中: columns 0-1
    widget.feedBytes("\x1b[1;1H");      // cursor back onto the wide cell
    //The block must cover the cell's full span: a one-cell rect (the
    //blink repaint and the damage rect of the old position both went
    //through that) left the right half of the inverted block on screen.
    QCOMPARE(widget.cursorRect().width(), widget.cellWidth() * 2);
}

void TestTerminal::TestLineModePromptPainted() {
    TerminalWidget widget;
    widget.setMode(TerminalWidget::Mode::Line);
    widget.show();
    QTest::qWaitForWindowExposed(&widget);
    //The '>' prompt heads the input line even before anything is typed.
    const QImage shot = widget.grab().toImage();
    QVERIFY2(InkAroundCentre(shot, widget, 0, 0) > 3,
             "line mode must paint the '>' prompt before the caret");
}

void TestTerminal::TestLineModePromptEcho() {
    TerminalWidget widget;
    widget.setMode(TerminalWidget::Mode::Line);
    QSignalSpy spy(&widget, &TerminalWidget::lineCommitted);
    QTest::keyClicks(&widget, "Bob");
    QTest::keyClick(&widget, Qt::Key_Return);
    //The echo (and thus the scrollback history) carries the prompt.
    QVERIFY(widget.screenText().find("> Bob") != std::string::npos);
    //The committed content itself stays prompt-free.
    QCOMPARE(spy.at(0).at(0).toString(), QString("Bob"));
}

//--- input modes / IME -------------------------------------------------------

void TestTerminal::TestLineEditorScalars() {
    LineEditor editor;
    editor.insertText("a\xE4\xB8\xAD""b");   //a 中 b
    QCOMPARE(editor.cursorScalarPosition(), 3);
    editor.moveCursor(-1);
    QCOMPARE(editor.cursorScalarPosition(), 2);
    //Backspace removes the whole previous scalar (中), never a half.
    editor.backspace();
    QCOMPARE(editor.line(), std::string("ab"));
    QCOMPARE(editor.cursorScalarPosition(), 1);
    editor.insertText("X");
    QCOMPARE(editor.line(), std::string("aXb"));
}

void TestTerminal::TestCharacterModeKeyPassthrough() {
    TerminalWidget widget;
    CaptureSink sink;
    widget.setByteSink(sink.sink());
    widget.setMode(TerminalWidget::Mode::Character);
    QTest::keyClicks(&widget, "abc");
    QCOMPARE(sink.bytes, std::string("abc"));
}

void TestTerminal::TestEnterEncodesCr() {
    TerminalWidget widget;
    CaptureSink sink;
    widget.setByteSink(sink.sink());
    widget.setMode(TerminalWidget::Mode::Character);
    QTest::keyClick(&widget, Qt::Key_Return);
    QCOMPARE(sink.bytes, std::string("\r"));
}

void TestTerminal::TestBackspaceDel() {
    TerminalWidget widget;
    CaptureSink sink;
    widget.setByteSink(sink.sink());
    widget.setMode(TerminalWidget::Mode::Character);
    QTest::keyClick(&widget, Qt::Key_Backspace);
    QCOMPARE(sink.bytes, std::string("\x7f"));
}

void TestTerminal::TestArrowCsi() {
    TerminalWidget widget;
    CaptureSink sink;
    widget.setByteSink(sink.sink());
    widget.setMode(TerminalWidget::Mode::Character);
    QTest::keyClick(&widget, Qt::Key_Up);
    QCOMPARE(sink.bytes, std::string("\x1b[A"));
}

void TestTerminal::TestCtrlCWithoutSelection() {
    //Without a selection Ctrl+C sends the interrupt (0x03) upstream.
    TerminalWidget widget;
    CaptureSink sink;
    widget.setByteSink(sink.sink());
    widget.setMode(TerminalWidget::Mode::Character);
    QTest::keyClick(&widget, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(sink.bytes, std::string("\x03"));

    //With a selection Ctrl+C copies instead — nothing is sent upstream.
    TerminalWidget selectWidget;
    CaptureSink selectSink;
    selectWidget.setByteSink(selectSink.sink());
    selectWidget.setMode(TerminalWidget::Mode::Character);
    selectWidget.feedBytes("hello world");
    QTest::mousePress(&selectWidget, Qt::LeftButton, Qt::NoModifier,
                      selectWidget.cellCenter(0, 0));
    QTest::mouseRelease(&selectWidget, Qt::LeftButton, Qt::NoModifier,
                        selectWidget.cellCenter(4, 0));
    QVERIFY(selectWidget.hasSelection());
    QTest::keyClick(&selectWidget, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(selectSink.bytes, std::string(""));
    QCOMPARE(selectWidget.selectedText(), QString("hello"));
}

void TestTerminal::TestLineModeEchoAndCommit() {
    TerminalWidget widget;
    CaptureSink sink;
    widget.setByteSink(sink.sink());
    widget.setMode(TerminalWidget::Mode::Line);
    QSignalSpy spy(&widget, &TerminalWidget::lineCommitted);
    QTest::keyClicks(&widget, "Bob");
    //The pending line is local: nothing on screen, nothing upstream.
    QCOMPARE(widget.pendingLine(), QString("Bob"));
    QVERIFY(widget.screenText().find("Bob") == std::string::npos);
    QCOMPARE(sink.bytes, std::string(""));
    QCOMPARE(spy.count(), 0);
    QTest::keyClick(&widget, Qt::Key_Return);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QString("Bob"));
    QVERIFY(widget.screenText().find("Bob") != std::string::npos);
    QCOMPARE(widget.pendingLine(), QString(""));
}

void TestTerminal::TestLineModeHistory() {
    TerminalWidget widget;
    widget.setMode(TerminalWidget::Mode::Line);
    QSignalSpy spy(&widget, &TerminalWidget::lineCommitted);
    QTest::keyClicks(&widget, "one");
    QTest::keyClick(&widget, Qt::Key_Return);
    QTest::keyClicks(&widget, "two");
    QTest::keyClick(&widget, Qt::Key_Return);
    QCOMPARE(spy.count(), 2);
    QTest::keyClick(&widget, Qt::Key_Up);
    QCOMPARE(widget.pendingLine(), QString("two"));
    QTest::keyClick(&widget, Qt::Key_Up);
    QCOMPARE(widget.pendingLine(), QString("one"));
    QTest::keyClick(&widget, Qt::Key_Down);
    QCOMPARE(widget.pendingLine(), QString("two"));
    QTest::keyClick(&widget, Qt::Key_Down);
    QCOMPARE(widget.pendingLine(), QString(""));   //draft restore
    QTest::keyClicks(&widget, "th");
    QCOMPARE(widget.pendingLine(), QString("th"));
}

void TestTerminal::TestLineModeBackspaceEditing() {
    TerminalWidget widget;
    widget.setMode(TerminalWidget::Mode::Line);
    QTest::keyClicks(&widget, "ab");
    QTest::keyClick(&widget, Qt::Key_Backspace);
    QCOMPARE(widget.pendingLine(), QString("a"));
    QTest::keyClick(&widget, Qt::Key_Left);
    QTest::keyClicks(&widget, "X");
    QCOMPARE(widget.pendingLine(), QString("Xa"));
}

void TestTerminal::TestEmptyLineEchoOnly() {
    TerminalWidget widget;
    widget.setMode(TerminalWidget::Mode::Line);
    QSignalSpy spy(&widget, &TerminalWidget::lineCommitted);
    QTest::keyClick(&widget, Qt::Key_Return);
    //Empty line: echo only — no commit.
    QCOMPARE(spy.count(), 0);
    QCOMPARE(widget.screenText(), std::string(""));
}

void TestTerminal::TestPaste() {
    //Character mode: paste goes upstream with CRLF collapsed to CR.
    TerminalWidget character;
    CaptureSink sink;
    character.setByteSink(sink.sink());
    character.setMode(TerminalWidget::Mode::Character);
    character.pasteText("hi");
    QCOMPARE(sink.bytes, std::string("hi"));
    character.pasteText("a\r\nb");
    QCOMPARE(sink.bytes, std::string("hia\rb"));
    //Line mode: newlines flatten to spaces (the editor holds one line).
    TerminalWidget line;
    line.setMode(TerminalWidget::Mode::Line);
    line.pasteText("a\r\nb");
    QCOMPARE(line.pendingLine(), QString("a b"));
}

void TestTerminal::TestImeCommit() {
    //Line mode: the commit string lands in the editor.
    TerminalWidget lineWidget;
    lineWidget.setMode(TerminalWidget::Mode::Line);
    QInputMethodEvent lineCommit;
    lineCommit.setCommitString(QStringLiteral("中"));
    QApplication::sendEvent(&lineWidget, &lineCommit);
    QVERIFY(lineWidget.pendingLine().contains(QStringLiteral("中")));

    //Character mode: the commit string is typed text upstream.
    TerminalWidget characterWidget;
    CaptureSink sink;
    characterWidget.setByteSink(sink.sink());
    characterWidget.setMode(TerminalWidget::Mode::Character);
    QInputMethodEvent characterCommit;
    characterCommit.setCommitString(QStringLiteral("中"));
    QApplication::sendEvent(&characterWidget, &characterCommit);
    QCOMPARE(sink.bytes, std::string("\xE4\xB8\xAD"));
}

//--- pty (real child processes) ------------------------------------------------

void TestTerminal::TestPtyEchoHello() {
    PtyProcess pty;
    QSignalSpy outputSpy(&pty, &PtyProcess::OutputReady);
    QSignalSpy finishedSpy(&pty, &PtyProcess::Finished);
    //"echo" is a cmd internal that never resets errorlevel: on hosts
    //whose AutoRun registry entry fails, `cmd /c echo hello` inherits
    //that errorlevel (observed exit 1). Pin the exit code explicitly.
    QVERIFY(pty.Start("cmd", {"/c", "echo hello&exit 0"}, "", 80, 24));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 15000);
    QCOMPARE(finishedSpy.at(0).at(0).toInt(), 0);
    QCOMPARE(finishedSpy.at(0).at(1).toBool(), false);
    QByteArray allOutput;
    for (const QVariantList& chunk : outputSpy)
        allOutput += chunk.at(0).toByteArray();
    QVERIFY(allOutput.contains("hello"));
}

void TestTerminal::TestPtyExitCode() {
    PtyProcess pty;
    QSignalSpy finishedSpy(&pty, &PtyProcess::Finished);
    QVERIFY(pty.Start("cmd", {"/c", "exit", "3"}, "", 80, 24));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 15000);
    QCOMPARE(finishedSpy.at(0).at(0).toInt(), 3);
    QCOMPARE(finishedSpy.at(0).at(1).toBool(), false);
}

void TestTerminal::TestPtyKill() {
    PtyProcess pty;
    QSignalSpy finishedSpy(&pty, &PtyProcess::Finished);
    QVERIFY(pty.Start("cmd", {}, "", 80, 24));   //interactive: stays up
    QVERIFY(pty.IsRunning());
    pty.Kill();
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 15000);
    QCOMPARE(finishedSpy.at(0).at(1).toBool(), true);
    QVERIFY(!pty.IsRunning());
    //Finished arrives exactly once: nothing more after the dust settles.
    QTest::qWait(500);
    QCOMPARE(finishedSpy.count(), 1);
}

void TestTerminal::TestPtyWriteRoundTrip() {
    PtyProcess pty;
    QSignalSpy finishedSpy(&pty, &PtyProcess::Finished);
    QVERIFY(pty.Start("cmd", {}, "", 80, 24));
    pty.Write("exit 7\r");
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 15000);
    QCOMPARE(finishedSpy.at(0).at(0).toInt(), 7);
    QCOMPARE(finishedSpy.at(0).at(1).toBool(), false);
}

QTEST_MAIN(TestTerminal)
#include "test_terminal.moc"
