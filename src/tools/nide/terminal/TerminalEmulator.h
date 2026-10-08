/*---
TerminalEmulator.h — thin owner of one libvterm instance: VT bytes in
(Feed), cell grid out, keyboard/paste encoding out. Deliberately
Qt-free so the emulation core is testable without widgets. Scrollback
lines pushed off-screen (sb_pushline callback) are kept in a bounded
deque; sb_popline answers scroll-back requests. Damage accumulates as
an inclusive row range the widget repaints and takes.
---*/
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <vterm.h>

namespace nlang {
namespace terminal {

//One grid cell: up to VTERM_MAX_CHARS_PER_CELL code points, a display
//width (advance to the next cell), the portable attribute set, and
//RGB colours with the palette conversion already applied — the widget
//never sees indexed colours.
struct TermCell {
    uint32_t chars[VTERM_MAX_CHARS_PER_CELL] = {0};
    int charCount = 0;
    int width = 1;
    bool bold = false;
    int underline = 0;      //VTERM_UNDERLINE_* value
    bool reverse = false;
    unsigned int foreground = 0x000000;
    unsigned int background = 0xFFFFFF;
};

//A scrollback line, one entry per column.
typedef std::vector<TermCell> TermLine;

class TerminalEmulator {
public:
    //Receives keyboard/paste reply bytes (what a real terminal would
    //send up the wire to the child). Empty sink discards.
    typedef std::function<void(const std::string&)> ByteSink;

    TerminalEmulator();
    ~TerminalEmulator();
    TerminalEmulator(const TerminalEmulator&) = delete;
    TerminalEmulator& operator=(const TerminalEmulator&) = delete;

    //Program output in (VT byte stream). Damage accumulates; callers
    //repaint TakeDamageRange() after feeding.
    void Feed(const std::string& vtBytes);
    void Resize(int columns, int rows);
    //Hard reset for a fresh session: clears screen + scrollback and
    //re-injects the palette (a reset restores factory colours).
    void Reset();

    int columns() const { return m_columns; }
    int rows() const { return m_rows; }
    //Cursor position is queried from the state on demand, NOT mirrored
    //from the movecursor callback: vterm_state_reset (behind Reset for
    //every fresh session) homes the real cursor without emitting one,
    //so a mirror goes stale exactly when the next session's prompt
    //needs the homed position.
    int cursorColumn() const;
    int cursorRow() const;

    //Visible-screen readout. Column iteration everywhere skips by
    //cell.width (a wide char's right half is never read individually).
    TermCell CellAt(int row, int column) const;
    //One row's text; a cell with no glyph reads back as spaces (one per
    //column it spans), trailing blanks are trimmed.
    std::string LineText(int row) const;
    std::string ScreenText() const;        //rows joined with '\n'

    const std::deque<TermLine>& scrollback() const { return m_scrollback; }

    //Inclusive damaged-row range since the last take; firstRow < 0
    //means "nothing damaged".
    void TakeDamageRange(int* firstRow, int* lastRow);
    bool cursorMoved() const { return m_cursorMoved; }
    void ClearCursorMoved() { m_cursorMoved = false; }

    //Keyboard/paste encoding — reply bytes go to the byte sink.
    void SetByteSink(ByteSink sink) { m_byteSink = std::move(sink); }
    void SendChar(uint32_t codePoint, VTermModifier modifiers);
    void SendKey(VTermKey key, VTermModifier modifiers);
    //One UTF-8 string as typed characters; '\n' becomes CR (the
    //terminal convention for multi-line paste).
    void SendText(const std::string& utf8Text);
    //Bracketed paste when the child enabled it, else plain SendText.
    void SendPaste(const std::string& utf8Text);

private:
    static int OnDamage(VTermRect rect, void* user);
    static int OnMoveCursor(VTermPos, VTermPos, int, void* user);
    static int OnPushLine(int cols, const VTermScreenCell* cells, void* user);
    static int OnPopLine(int cols, VTermScreenCell* cells, void* user);

    static const VTermScreenCallbacks kScreenCallbacks;

    void NoteDamageRow(int row);
    TermCell ConvertCell(const VTermScreenCell& cell) const;

    VTerm* m_vt = nullptr;
    VTermScreen* m_screen = nullptr;
    VTermState* m_state = nullptr;   //for cursor queries (cursorRow())
    int m_columns = 0;
    int m_rows = 0;
    int m_firstDamaged = -1;
    int m_lastDamaged = -1;
    bool m_cursorMoved = false;
    std::deque<TermLine> m_scrollback;
    ByteSink m_byteSink;
};

} // namespace terminal
} // namespace nlang
