/*---
LineEditor.h — the line-mode editing model: one UTF-8 line, a cursor
that always sits on scalar boundaries, and a bounded history with draft
restore. Pure std, no Qt, so the model is testable standalone; the
widget renders and feeds it key events.
---*/
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace nlang {
namespace terminal {

class LineEditor {
public:
    void insertText(const std::string& utf8Text);
    void backspace();                    //erase the previous scalar
    void moveCursor(int deltaScalars);   //cursor stays on scalar starts
    void clear();                        //line + cursor, history kept
    const std::string& line() const { return m_line; }
    std::size_t cursorByte() const { return m_cursorByte; }
    int cursorScalarPosition() const;
    bool empty() const { return m_line.empty(); }
    bool historyPrev();                  //false = no older entry
    bool historyNext();                  //false = already on live line
    void commit();                       //non-empty line -> history

private:
    static constexpr int kMaxHistory = 50;

    void loadHistoryEntry();

    std::string m_line;
    std::size_t m_cursorByte = 0;
    //m_historyIndex: -1 = the live line, else an index into m_history.
    int m_historyIndex = -1;
    std::string m_draft;   //live line stashed by the first historyPrev
    std::vector<std::string> m_history;
};

} // namespace terminal
} // namespace nlang
