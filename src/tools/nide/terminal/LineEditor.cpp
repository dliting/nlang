/*---
LineEditor.cpp — see the header for the model contract. All byte
walking treats a UTF-8 continuation byte as (byte & 0xC0) == 0x80; the
cursor offset therefore always lands on a scalar start.
---*/
#include "LineEditor.h"

namespace nlang {
namespace terminal {
namespace {

bool IsContinuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

//Byte offset of the scalar start immediately before pos (pos > 0).
std::size_t PreviousScalarStart(const std::string& text, std::size_t pos) {
    std::size_t at = pos - 1;
    while (at > 0 && IsContinuation(text[at]))
        --at;
    return at;
}

//Byte offset one scalar past pos (pos on a scalar start, <= size).
std::size_t NextScalarEnd(const std::string& text, std::size_t pos) {
    std::size_t at = pos + 1;
    while (at < text.size() && IsContinuation(text[at]))
        ++at;
    return at;
}

} // namespace

void LineEditor::insertText(const std::string& utf8Text) {
    m_line.insert(m_cursorByte, utf8Text);
    m_cursorByte += utf8Text.size();
}

void LineEditor::backspace() {
    if (m_cursorByte == 0) return;
    const std::size_t start = PreviousScalarStart(m_line, m_cursorByte);
    m_line.erase(start, m_cursorByte - start);
    m_cursorByte = start;
}

void LineEditor::moveCursor(int deltaScalars) {
    if (deltaScalars < 0) {
        for (int i = 0; i < -deltaScalars && m_cursorByte > 0; ++i)
            m_cursorByte = PreviousScalarStart(m_line, m_cursorByte);
    } else {
        for (int i = 0; i < deltaScalars
             && m_cursorByte < m_line.size(); ++i)
            m_cursorByte = NextScalarEnd(m_line, m_cursorByte);
    }
}

void LineEditor::clear() {
    m_line.clear();
    m_cursorByte = 0;
    m_historyIndex = -1;
    m_draft.clear();
}

int LineEditor::cursorScalarPosition() const {
    int count = 0;
    for (std::size_t i = 0; i < m_cursorByte; ++i)
        if (!IsContinuation(m_line[i]))
            ++count;
    return count;
}

bool LineEditor::historyPrev() {
    if (m_history.empty()) return false;
    if (m_historyIndex == -1) {
        //First step back: stash the live line as the draft.
        m_draft = m_line;
        m_historyIndex = static_cast<int>(m_history.size()) - 1;
    } else if (m_historyIndex > 0) {
        --m_historyIndex;
    } else {
        return false;   //already at the oldest entry
    }
    loadHistoryEntry();
    return true;
}

bool LineEditor::historyNext() {
    if (m_historyIndex == -1) return false;   //already on the live line
    if (m_historyIndex
        < static_cast<int>(m_history.size()) - 1) {
        ++m_historyIndex;
        loadHistoryEntry();
    } else {
        //Past the newest entry: back on the live line (draft restore).
        m_historyIndex = -1;
        m_line = m_draft;
        m_cursorByte = m_line.size();
        m_draft.clear();
    }
    return true;
}

void LineEditor::commit() {
    if (m_line.empty()) return;
    //A repeat of the newest entry is not worth a history slot.
    if (!m_history.empty() && m_history.back() == m_line) return;
    m_history.push_back(m_line);
    if (static_cast<int>(m_history.size()) > kMaxHistory)
        m_history.erase(m_history.begin());
}

void LineEditor::loadHistoryEntry() {
    m_line = m_history[static_cast<std::size_t>(m_historyIndex)];
    m_cursorByte = m_line.size();
}

} // namespace terminal
} // namespace nlang
