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
#include <QTabWidget>
#include <QVBoxLayout>
#include <QAction>
#include <QCoreApplication>
#include <QMenu>
#include <QWheelEvent>
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
    void TestHiddenPageFeedKeepsOutput();
    void TestHiddenPageNestedHostKeepsOutput();
    void TestColumnsRowsFromSize();
    void TestSelectionAndCopy();
    void TestBlankCellReadsAsSpace();
    void TestWideCharPaintsBothCells();
    void TestWideCharPaintsAtOffset();
    void TestWideCharPendingLinePaints();
    void TestWideCharCursorBlockSpansCell();
    void TestLineModePromptPainted();
    void TestLineModePromptEcho();
    void TestLightThemeBackgroundAndText();

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

    //--- clipboard family / context menu / select all / zoom ---
    void TestCtrlShiftCVariants();
    void TestShiftInsertIntercepted();
    void TestContextMenuActions();
    void TestSelectAllViewport();
    void TestCtrlWheelZoomSignal();
    void TestSetTerminalFontPtRelayouts();

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
    //SGR 31 = palette index 1 (light-scheme red 0xCD3131), converted to
    //RGB.
    QCOMPARE(emulator.CellAt(0, 0).foreground, 0xCD3131u);
    const TermCell bold = emulator.CellAt(0, 1);
    QVERIFY(bold.bold);
    QCOMPARE(bold.underline, int(VTERM_UNDERLINE_SINGLE));
    QVERIFY(bold.reverse);
    //After SGR 0 everything is back to the palette defaults (black).
    QCOMPARE(emulator.CellAt(0, 2).foreground, 0x000000u);
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
    QCOMPARE(emulator.CellAt(0, 0).foreground, 0xCD3131u);
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

//Output can arrive while the terminal's page is hidden (a debug session
//feeds the shared terminal from the debug tab), and no resize event
//will follow: QStackedLayout (StackOne) assigns geometry to the
//current page only, so a never-current page keeps its constructed
//placeholder size (640x480 here — bigger than the real page, which is
//the worst case: fed there, the output rides the first-show
//relayout's bottom-anchored shrink into the scrollback and the visible
//screen comes up blank — the "output vanished after a breakpoint"
//report). The feed must adopt the real page geometry first.
void TestTerminal::TestHiddenPageFeedKeepsOutput() {
    QTabWidget tabs;
    tabs.addTab(new QWidget, "other");
    TerminalWidget* terminal = new TerminalWidget;
    tabs.addTab(terminal, "terminal");   //never the current page yet
    tabs.resize(320, 140);   //the real page is well below the placeholder
    tabs.show();
    QTest::qWaitForWindowExposed(&tabs);
    terminal->feedUtf8("one\n");   //fed while hidden
    terminal->feedUtf8("two\n");
    terminal->feedUtf8("three\n");
    //The hidden page adopted the rect the stack assigned to the
    //CURRENT page — the exact mirror contract (any wrong-but-smaller
    //rect would pass a plain upper bound).
    QCOMPARE(terminal->size(), tabs.widget(0)->size());
    QVERIFY(terminal->rows() != 24);
    //First show: the relayout must find the grid already right, so the
    //fed lines are still on the visible screen.
    tabs.setCurrentIndex(1);
    const std::string screen = terminal->screenText();
    QVERIFY(screen.find("one") != std::string::npos);
    QVERIFY(screen.find("three") != std::string::npos);
}

//nide's real embedding is two layout levels deep: page (its layout)
//→ terminalHost (its layout) → terminal. A hidden widget receives no
//resize event, so an inner layout never runs on its own — a sync that
//only activates the PAGE's layout hands the rect to the host and stops
//there, leaving the terminal on its constructed size and the fed
//output wrapped at a fictional grid. The sync must propagate the rect
//down every level of the chain.
void TestTerminal::TestHiddenPageNestedHostKeepsOutput() {
    QTabWidget tabs;
    tabs.addTab(new QWidget, "other");
    TerminalWidget* terminal = new TerminalWidget;
    QWidget* host = new QWidget;
    QVBoxLayout* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);
    hostLayout->addWidget(terminal);
    QWidget* page = new QWidget;
    QVBoxLayout* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    pageLayout->addWidget(host);
    tabs.addTab(page, "terminal");   //never the current page yet
    tabs.resize(320, 140);
    tabs.show();
    QTest::qWaitForWindowExposed(&tabs);
    terminal->feedUtf8("one\n");   //fed while hidden, two levels deep
    terminal->feedUtf8("two\n");
    terminal->feedUtf8("three\n");
    //The rect reached through BOTH layout levels: the terminal holds
    //exactly the current page's assigned size.
    QCOMPARE(terminal->size(), tabs.widget(0)->size());
    QVERIFY(terminal->rows() != 24);
    tabs.setCurrentIndex(1);
    const std::string screen = terminal->screenText();
    QVERIFY(screen.find("one") != std::string::npos);
    QVERIFY(screen.find("three") != std::string::npos);
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
    const QColor background(0xFF, 0xFF, 0xFF);   //kDefaultBackground
    const int halfBandW = widget.cellWidth() / 4;
    const int halfBandH = widget.cellHeight() / 4;
    int ink = 0;
    for (int x = centre.x() - halfBandW; x <= centre.x() + halfBandW; ++x)
        for (int y = centre.y() - halfBandH; y <= centre.y() + halfBandH; ++y)
            if (shot.pixelColor(x, y) != background)
                ++ink;
    return ink;
}

//Counts dark pixels (all three channels below kDarkChannelMax) in a small
//window around a cell centre: the ink probe for the light-theme test. The
//old light-grey 0xCCCCCC foreground would leave no dark pixel at all, so this
//probe only goes green once the text is actually black.
static int DarkInkAroundCentre(const QImage& shot,
                               const TerminalWidget& widget,
                               int column, int row) {
    //A channel strictly below this reads as dark ink; black text (0x00)
    //lands well under it, and the old light-grey foreground (0xCC) does not.
    constexpr int kDarkChannelMax = 0x80;
    const QPoint centre = widget.cellCenter(column, row);
    const int halfBandW = widget.cellWidth() / 4;
    const int halfBandH = widget.cellHeight() / 4;
    int ink = 0;
    for (int x = centre.x() - halfBandW; x <= centre.x() + halfBandW; ++x)
        for (int y = centre.y() - halfBandH; y <= centre.y() + halfBandH; ++y) {
            const QRgb pixel = shot.pixel(x, y);
            const int r = (pixel >> 16) & 0xFF;
            const int g = (pixel >> 8) & 0xFF;
            const int b = pixel & 0xFF;
            if (r < kDarkChannelMax && g < kDarkChannelMax && b < kDarkChannelMax)
                ++ink;
        }
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

//The terminal is black-on-white to match the rest of nide's light windows:
//the background is pure white and the default text is black. Both halves are
//checked so a partial flip (white bg but grey text, or black bg but black
//text) cannot slip through: a far corner must be pure white (the old
//near-black 0x0C0C0C would fail this), and the glyph must carry dark ink
//(the old light-grey 0xCCCCCC foreground would leave no dark pixel).
void TestTerminal::TestLightThemeBackgroundAndText() {
    TerminalWidget widget;
    widget.show();
    QTest::qWaitForWindowExposed(&widget);
    widget.feedBytes("Hi");
    const QImage shot = widget.grab().toImage();
    //A corner well clear of any glyph (the text "Hi" sits at the top-left,
    //so the bottom-right is blank) must be pure white. Compare only the
    //RGB channels: pixel() returns 0xAARRGGBB, so an opaque white pixel
    //reads 0xFFFFFFFF (alpha FF) and would mismatch a bare 0xFFFFFF.
    const QRgb corner = shot.pixel(shot.width() - 3, shot.height() - 3);
    QCOMPARE((corner & 0x00FFFFFFu), 0x00FFFFFFu);
    QVERIFY2(DarkInkAroundCentre(shot, widget, 0, 0) > 3,
             "foreground text must be dark ink on the light background");
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

//--- clipboard family / context menu / select all / zoom -----------------------

void TestTerminal::TestCtrlShiftCVariants() {
    //Ctrl+Shift+C ALWAYS copies and never interrupts; so does
    //Ctrl+Insert. Both are tested with and without a selection.
    TerminalWidget widget;
    CaptureSink sink;
    widget.setByteSink(sink.sink());
    widget.setMode(TerminalWidget::Mode::Character);
    QTest::keyClick(&widget, Qt::Key_C,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QTest::keyClick(&widget, Qt::Key_Insert, Qt::ControlModifier);
    QCOMPARE(sink.bytes, std::string(""));   //nothing went upstream
    widget.feedBytes("hello world");
    QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier,
                      widget.cellCenter(0, 0));
    QTest::mouseRelease(&widget, Qt::LeftButton, Qt::NoModifier,
                        widget.cellCenter(4, 0));
    QVERIFY(widget.hasSelection());
    QTest::keyClick(&widget, Qt::Key_C,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QTest::keyClick(&widget, Qt::Key_Insert, Qt::ControlModifier);
    QCOMPARE(sink.bytes, std::string(""));
    QCOMPARE(widget.selectedText(), QString("hello"));
}

void TestTerminal::TestShiftInsertIntercepted() {
    //Shift+Insert must PASTE, never travel to the child as the INSERT
    //key. The live clipboard is not controllable on this host (the
    //clipboard manager keeps it locked), so assert the interception
    //self-calibrated against what a raw INSERT key would encode.
    TerminalWidget widget;
    CaptureSink sink;
    widget.setByteSink(sink.sink());
    widget.setMode(TerminalWidget::Mode::Character);
    QTest::keyClick(&widget, Qt::Key_Insert, Qt::ShiftModifier);
    //The reference encoding must be non-empty for the assertion to
    //mean anything — a silently empty reference would vacuously pass.
    TerminalEmulator reference;
    CaptureSink referenceSink;
    reference.SetByteSink(referenceSink.sink());
    reference.SendKey(VTERM_KEY_INS, VTERM_MOD_NONE);
    QVERIFY(!referenceSink.bytes.empty());
    QVERIFY(sink.bytes.find(referenceSink.bytes)
            == std::string::npos);
}

void TestTerminal::TestContextMenuActions() {
    TerminalWidget widget;   //default mode: Idle
    widget.feedBytes("hello");
    QMenu* menu = widget.buildContextMenu();
    const QList<QAction*> actions = menu->actions();
    QCOMPARE(actions.size(), 4);   //Copy, Paste, separator, Select All
    QCOMPARE(actions.at(0)->text(), QStringLiteral("&Copy"));
    QCOMPARE(actions.at(1)->text(), QStringLiteral("&Paste"));
    QCOMPARE(actions.at(3)->text(), QStringLiteral("Select &All"));
    //Idle terminal: paste is disabled regardless of the clipboard
    //(pasteText would be a silent no-op); copy needs a selection.
    QVERIFY(!actions.at(0)->isEnabled());
    QVERIFY(!actions.at(1)->isEnabled());
    QVERIFY(actions.at(3)->isEnabled());
    actions.at(3)->trigger();
    QVERIFY(widget.hasSelection());
    delete menu;   //live shortcut registered while the menu exists
    //A selection flips Copy to enabled on the next open.
    QMenu* withSelection = widget.buildContextMenu();
    QVERIFY(withSelection->actions().at(0)->isEnabled());
    delete withSelection;
}

void TestTerminal::TestSelectAllViewport() {
    TerminalWidget widget;
    widget.feedUtf8("one\n");
    widget.feedUtf8("two\n");
    widget.feedUtf8("three");   //no trailing newline: the last content
                                //line is text, not a blank cursor row
    widget.selectAll();
    QVERIFY(widget.hasSelection());
    //The selection spans every screen row (the emulator default grid;
    //rows beyond the fed text read back as empty lines).
    const QStringList lines =
        widget.selectedText().split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), widget.rows());
    QCOMPARE(lines.first(), QString("one"));
    QCOMPARE(lines.at(2), QString("three"));
    QVERIFY(lines.last().isEmpty());
    //Select All jumps back to the live screen first: after scrolling
    //into the scrollback the selection reads the CURRENT screen (L39),
    //never the scrolled view (L00 is scrollback).
    TerminalWidget scrolled;
    for (int i = 0; i < 40; ++i) {
        char line[16];
        std::snprintf(line, sizeof(line), "L%02d\r\n", i);
        scrolled.feedBytes(line);
    }
    QWheelEvent back(scrolled.rect().center(), QPointF(), 240,
                     Qt::NoButton, Qt::NoModifier, Qt::Vertical);
    QCoreApplication::sendEvent(&scrolled, &back);
    scrolled.selectAll();
    QVERIFY(scrolled.selectedText().contains(QString("L39")));
    QVERIFY(!scrolled.selectedText().contains(QString("L00")));
}

void TestTerminal::TestCtrlWheelZoomSignal() {
    TerminalWidget widget;
    QSignalSpy spy(&widget, &TerminalWidget::fontSizeZoomRequested);
    QWheelEvent zoomIn(widget.rect().center(), QPointF(), 120,
                      Qt::NoButton, Qt::ControlModifier, Qt::Vertical);
    QCoreApplication::sendEvent(&widget, &zoomIn);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toInt(), 1);
    QWheelEvent zoomOut(widget.rect().center(), QPointF(), -120,
                        Qt::NoButton, Qt::ControlModifier, Qt::Vertical);
    QCoreApplication::sendEvent(&widget, &zoomOut);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toInt(), -1);
    //A plain wheel never zooms (it scrolls the view instead).
    QWheelEvent plain(widget.rect().center(), QPointF(), 120,
                      Qt::NoButton, Qt::NoModifier, Qt::Vertical);
    QCoreApplication::sendEvent(&widget, &plain);
    QCOMPARE(spy.count(), 2);
}

void TestTerminal::TestSetTerminalFontPtRelayouts() {
    TerminalWidget widget;
    widget.show();
    QTest::qWaitForWindowExposed(&widget);
    widget.resize(400, 300);
    const int narrowCell = widget.cellWidth();
    const int narrowColumns = widget.columns();
    widget.setTerminalFontPt(20);
    QVERIFY(widget.cellWidth() > narrowCell);
    QVERIFY(widget.columns() < narrowColumns);
    //Same size again is a no-op: the early return keeps metrics.
    const int wideCell = widget.cellWidth();
    widget.setTerminalFontPt(20);
    QCOMPARE(widget.cellWidth(), wideCell);
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
