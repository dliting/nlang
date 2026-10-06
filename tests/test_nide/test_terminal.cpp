// Embedded terminal tests: TerminalEmulator over the vendored libvterm
// (cell readout, damage, scrollback, key encoding), the TerminalWidget
// render/input model, and PtyProcess with real child processes.

#include <QtTest>

#include "terminal/TerminalEmulator.h"

#include <QApplication>
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
    void TestResize();
    void TestSendCharEncodes();
    void TestSendKeyEncodes();
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

void TestTerminal::TestResize() {
    TerminalEmulator emulator;
    emulator.Resize(100, 40);
    QCOMPARE(emulator.columns(), 100);
    QCOMPARE(emulator.rows(), 40);
    emulator.Feed("\x1b[40;100H" "x");   // bottom-right corner
    QCOMPARE(emulator.CellAt(39, 99).chars[0], uint32_t('x'));
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

QTEST_MAIN(TestTerminal)
#include "test_terminal.moc"
