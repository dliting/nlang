/*---
TerminalEmulator.cpp — see the header for the component contract. The
static screen callbacks (damage/movecursor/pushline/popline) are the
only libvterm entry points with state implications; everything else is
straight API calls.
---*/
#include "TerminalEmulator.h"

#include <cstring>

#include "TerminalPalette.h"

namespace nlang {
namespace terminal {
namespace {

constexpr int kInitialColumns = 80;
constexpr int kInitialRows = 24;
constexpr std::size_t kMaxScrollbackLines = 10000;

//Appends one code point as UTF-8 (the cell grid stores code points).
void AppendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

//Decodes the next UTF-8 scalar at pos; false at end of string. Tolerant
//of malformed input (consumes one byte, yields U+FFFD) — this feeds the
//keyboard path, where our own Qt strings are always valid UTF-8.
bool NextUtf8Scalar(const std::string& text, std::size_t& pos,
                    uint32_t& codePoint) {
    if (pos >= text.size()) return false;
    const unsigned char lead =
        static_cast<unsigned char>(text[pos]);
    if (lead < 0x80) {
        codePoint = lead;
        pos += 1;
        return true;
    }
    const int extra = (lead & 0xE0) == 0xC0 ? 1
                    : (lead & 0xF0) == 0xE0 ? 2
                    : (lead & 0xF8) == 0xF0 ? 3 : 0;
    if (extra == 0) {
        //Bad lead byte: consume it, yield the replacement character.
        codePoint = 0xFFFD;
        pos += 1;
        return true;
    }
    codePoint = lead & (0x3F >> extra);
    for (int i = 1; i <= extra; ++i) {
        const std::size_t at = pos + static_cast<std::size_t>(i);
        if (at >= text.size()) {
            codePoint = 0xFFFD;   //truncated sequence
            pos += 1;
            return true;
        }
        const unsigned char byte =
            static_cast<unsigned char>(text[at]);
        if ((byte & 0xC0) != 0x80) {
            codePoint = 0xFFFD;   //invalid continuation byte
            pos += 1;
            return true;
        }
        codePoint = (codePoint << 6) | (byte & 0x3F);
    }
    pos += static_cast<std::size_t>(extra) + 1;
    return true;
}

} // namespace

const VTermScreenCallbacks TerminalEmulator::kScreenCallbacks = {
    &TerminalEmulator::OnDamage,     // damage
    nullptr,                         // moverect (row damage suffices)
    &TerminalEmulator::OnMoveCursor, // movecursor
    nullptr,                         // settermprop
    nullptr,                         // bell
    nullptr,                         // resize
    &TerminalEmulator::OnPushLine,   // sb_pushline
    &TerminalEmulator::OnPopLine,    // sb_popline
    nullptr,                         // sb_clear
    nullptr,                         // sb_pushline4 (not opted into)
};

TerminalEmulator::TerminalEmulator() {
    m_vt = vterm_new(kInitialRows, kInitialColumns);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt,
        [](const char* bytes, std::size_t len, void* user) {
            TerminalEmulator* self = static_cast<TerminalEmulator*>(user);
            if (self->m_byteSink)
                self->m_byteSink(std::string(bytes, len));
        }, this);
    m_screen = vterm_obtain_screen(m_vt);
    vterm_screen_set_callbacks(m_screen, &kScreenCallbacks, this);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_ROW);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_reset(m_screen, 1);
    //A hard reset restores libvterm's factory colours, so the palette
    //goes in after it (and after every Reset()).
    ApplyTerminalPalette(m_vt);
    ApplyTerminalDefaultColors(m_screen);
    m_columns = kInitialColumns;
    m_rows = kInitialRows;
}

TerminalEmulator::~TerminalEmulator() {
    vterm_free(m_vt);
}

void TerminalEmulator::Feed(const std::string& vtBytes) {
    vterm_input_write(m_vt, vtBytes.data(), vtBytes.size());
    vterm_screen_flush_damage(m_screen);
}

void TerminalEmulator::Resize(int columns, int rows) {
    if (columns == m_columns && rows == m_rows) return;
    vterm_set_size(m_vt, rows, columns);
    vterm_screen_flush_damage(m_screen);
    m_columns = columns;
    m_rows = rows;
    //Reflow changed every row; the widget relayouts and repaints all.
    m_firstDamaged = 0;
    m_lastDamaged = rows - 1;
    m_cursorMoved = true;
}

void TerminalEmulator::Reset() {
    m_scrollback.clear();
    vterm_screen_reset(m_screen, 1);
    vterm_screen_flush_damage(m_screen);
    ApplyTerminalPalette(m_vt);
    ApplyTerminalDefaultColors(m_screen);
    m_firstDamaged = 0;
    m_lastDamaged = m_rows - 1;
    m_cursorMoved = true;
}

TermCell TerminalEmulator::CellAt(int row, int column) const {
    if (row < 0) {
        //Negative rows count back into the scrollback (-1 = newest).
        const std::size_t index =
            m_scrollback.size() - static_cast<std::size_t>(-row);
        if (index >= m_scrollback.size()) return TermCell();
        const TermLine& line = m_scrollback[index];
        if (column < 0
            || column >= static_cast<int>(line.size()))
            return TermCell();
        return line[static_cast<std::size_t>(column)];
    }
    if (row >= m_rows || column < 0 || column >= m_columns)
        return TermCell();
    VTermScreenCell cell;
    if (!vterm_screen_get_cell(
            m_screen, VTermPos{row, column}, &cell))
        return TermCell();
    return ConvertCell(cell);
}

std::string TerminalEmulator::LineText(int row) const {
    std::string out;
    if (row < 0 || row >= m_rows) return out;
    int column = 0;
    while (column < m_columns) {
        VTermScreenCell cell;
        if (!vterm_screen_get_cell(
                m_screen, VTermPos{row, column}, &cell))
            break;
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL
                        && cell.chars[i] != 0; ++i)
            AppendUtf8(out, cell.chars[i]);
        column += cell.width > 0 ? cell.width : 1;
    }
    //Trailing blank cells are not part of the line's content.
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string TerminalEmulator::ScreenText() const {
    //Trailing blank rows are not content: a freshly reset screen reads
    //back as "" (not a run of newlines).
    std::string out;
    int lastNonEmptyRow = -1;
    for (int row = 0; row < m_rows; ++row)
        if (!LineText(row).empty()) lastNonEmptyRow = row;
    for (int row = 0; row <= lastNonEmptyRow; ++row) {
        if (row > 0) out.push_back('\n');
        out += LineText(row);
    }
    return out;
}

void TerminalEmulator::TakeDamageRange(int* firstRow, int* lastRow) {
    *firstRow = m_firstDamaged;
    *lastRow = m_lastDamaged;
    m_firstDamaged = -1;
    m_lastDamaged = -1;
}

void TerminalEmulator::SendChar(uint32_t codePoint,
                                VTermModifier modifiers) {
    vterm_keyboard_unichar(m_vt, codePoint, modifiers);
}

void TerminalEmulator::SendKey(VTermKey key, VTermModifier modifiers) {
    vterm_keyboard_key(m_vt, key, modifiers);
}

void TerminalEmulator::SendText(const std::string& utf8Text) {
    std::size_t pos = 0;
    uint32_t codePoint = 0;
    while (NextUtf8Scalar(utf8Text, pos, codePoint)) {
        uint32_t toSend = codePoint;
        if (codePoint == '\n' || codePoint == '\r') {
            toSend = '\r';   //the terminal's line terminator
            //A CRLF pair collapses into one CR (paste normalization).
            std::size_t peek = pos;
            uint32_t next = 0;
            if (codePoint == '\r'
                && NextUtf8Scalar(utf8Text, peek, next) && next == '\n')
                pos = peek;
        }
        SendChar(toSend, VTERM_MOD_NONE);
    }
}

void TerminalEmulator::SendPaste(const std::string& utf8Text) {
    vterm_keyboard_start_paste(m_vt);
    SendText(utf8Text);
    vterm_keyboard_end_paste(m_vt);
}

void TerminalEmulator::NoteDamageRow(int row) {
    if (row < 0 || row >= m_rows) return;
    if (m_firstDamaged < 0) {
        m_firstDamaged = row;
        m_lastDamaged = row;
    } else if (row < m_firstDamaged) {
        m_firstDamaged = row;
    } else if (row > m_lastDamaged) {
        m_lastDamaged = row;
    }
}

TermCell TerminalEmulator::ConvertCell(
    const VTermScreenCell& cell) const {
    TermCell out;
    int count = 0;
    while (count < VTERM_MAX_CHARS_PER_CELL && cell.chars[count] != 0) {
        out.chars[count] = cell.chars[count];
        ++count;
    }
    out.charCount = count;
    out.width = cell.width > 0 ? cell.width : 1;
    out.bold = cell.attrs.bold != 0;
    out.underline = cell.attrs.underline;
    out.reverse = cell.attrs.reverse != 0;
    VTermColor foreground = cell.fg;
    VTermColor background = cell.bg;
    vterm_screen_convert_color_to_rgb(m_screen, &foreground);
    vterm_screen_convert_color_to_rgb(m_screen, &background);
    out.foreground = (unsigned int(foreground.rgb.red) << 16)
                   | (unsigned int(foreground.rgb.green) << 8)
                   | unsigned int(foreground.rgb.blue);
    out.background = (unsigned int(background.rgb.red) << 16)
                   | (unsigned int(background.rgb.green) << 8)
                   | unsigned int(background.rgb.blue);
    return out;
}

int TerminalEmulator::OnDamage(VTermRect rect, void* user) {
    TerminalEmulator* self = static_cast<TerminalEmulator*>(user);
    for (int row = rect.start_row; row < rect.end_row; ++row)
        self->NoteDamageRow(row);
    return 1;
}

int TerminalEmulator::OnMoveCursor(VTermPos pos, VTermPos,
                                   int, void* user) {
    TerminalEmulator* self = static_cast<TerminalEmulator*>(user);
    self->m_cursorColumn = pos.col;
    self->m_cursorRow = pos.row;
    self->m_cursorMoved = true;
    return 1;
}

int TerminalEmulator::OnPushLine(int cols,
                                 const VTermScreenCell* cells,
                                 void* user) {
    TerminalEmulator* self = static_cast<TerminalEmulator*>(user);
    TermLine line;
    line.reserve(static_cast<std::size_t>(cols));
    for (int col = 0; col < cols; ++col)
        line.push_back(self->ConvertCell(
            cells[static_cast<std::size_t>(col)]));
    self->m_scrollback.push_back(std::move(line));
    if (self->m_scrollback.size() > kMaxScrollbackLines)
        self->m_scrollback.pop_front();
    return 1;
}

int TerminalEmulator::OnPopLine(int cols, VTermScreenCell* cells,
                                void* user) {
    TerminalEmulator* self = static_cast<TerminalEmulator*>(user);
    if (self->m_scrollback.empty()) return 0;
    const TermLine& line = self->m_scrollback.back();
    for (int col = 0; col < cols; ++col) {
        VTermScreenCell& out = cells[static_cast<std::size_t>(col)];
        std::memset(&out, 0, sizeof(out));
        const TermCell source =
            col < static_cast<int>(line.size())
                ? line[static_cast<std::size_t>(col)] : TermCell();
        for (int i = 0; i < source.charCount
                        && i < VTERM_MAX_CHARS_PER_CELL; ++i)
            out.chars[i] = source.chars[i];
        out.width = static_cast<char>(source.width);
        out.attrs.bold = source.bold ? 1 : 0;
        out.attrs.underline =
            static_cast<unsigned int>(source.underline);
        out.attrs.reverse = source.reverse ? 1 : 0;
        SetTerminalColor(&out.fg, source.foreground);
        SetTerminalColor(&out.bg, source.background);
    }
    self->m_scrollback.pop_back();
    return 1;
}

} // namespace terminal
} // namespace nlang
