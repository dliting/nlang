/*---
TokenView.cpp — line-remainder slicing for the four input entries
(see the header for the shared-cursor rationale).
---*/
#include "TokenView.h"

namespace nlang {
namespace {

bool IsSpaceByte(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

//Decode one UTF-8 scalar at s[pos..). Returns false on any malformed
//sequence: bad lead byte, truncated continuation, overlong form,
//surrogate half, or a value above U+10FFFF.
bool DecodeUtf8Scalar(const std::string& s, size_t pos, uint32_t& value,
                      size_t& length) {
    const auto Byte = [&](size_t i) {
        return static_cast<unsigned char>(s[pos + i]);
    };
    const unsigned char lead = Byte(0);
    size_t need = 0;
    uint32_t v = 0;
    if (lead < 0x80) { value = lead; length = 1; return true; }
    if ((lead & 0xE0) == 0xC0) { need = 1; v = lead & 0x1Fu; }
    else if ((lead & 0xF0) == 0xE0) { need = 2; v = lead & 0x0Fu; }
    else if ((lead & 0xF8) == 0xF0) { need = 3; v = lead & 0x07u; }
    else return false;
    for (size_t i = 1; i <= need; ++i) {
        if (pos + i >= s.size()) return false;
        const unsigned char b = Byte(i);
        if ((b & 0xC0) != 0x80) return false;
        v = (v << 6) | (b & 0x3Fu);
    }
    //Overlong forms encode a smaller value than the sequence length
    //allows — reject them so each scalar has exactly one encoding.
    static const uint32_t kMinForNeed[] = {0x80, 0x800, 0x10000};
    if (v < kMinForNeed[need - 1]) return false;
    if (v > 0x10FFFF) return false;
    if (v >= 0xD800 && v <= 0xDFFF) return false;   //surrogate halves
    value = v;
    length = need + 1;
    return true;
}

} // namespace

InputReadStatus TokenView::ReadLine(std::string& outLine) {
    if (!m_lineLive) {
        std::string next;
        const InputReadStatus st = m_source.PullLine(next);
        if (st != InputReadStatus::Ok) {
            outLine.clear();   //Eof: "" sentinel; NoChannel: raised by
            return st;         //the callback, outLine unused
        }
        m_line = std::move(next);
        m_pos = 0;
    }
    //The remainder is handed out in full — even an empty remainder
    //reads as "" once before the next pull (C++ getline mirror).
    m_lineLive = false;
    outLine = m_line.substr(m_pos);
    return InputReadStatus::Ok;
}

InputReadStatus TokenView::ReadToken(std::string& outToken) {
    const InputReadStatus st = SkipWhitespace();
    if (st != InputReadStatus::Ok) return st;
    const size_t start = m_pos;
    while (m_pos < m_line.size() && !IsSpaceByte(m_line[m_pos]))
        ++m_pos;
    outToken = m_line.substr(start, m_pos - start);
    return InputReadStatus::Ok;
}

InputReadStatus TokenView::ReadChar(uint32_t& outChar, bool& invalidUtf8) {
    invalidUtf8 = false;
    const InputReadStatus st = SkipWhitespace();
    if (st != InputReadStatus::Ok) return st;
    uint32_t value = 0;
    size_t length = 0;
    if (!DecodeUtf8Scalar(m_line, m_pos, value, length)) {
        invalidUtf8 = true;   //cursor untouched: the caller raises
        return InputReadStatus::Ok;
    }
    m_pos += length;
    outChar = value;
    return InputReadStatus::Ok;
}

bool TokenView::HasInput() {
    if (m_lineLive) return true;
    return m_source.HasMore();
}

InputReadStatus TokenView::SkipWhitespace() {
    for (;;) {
        while (m_lineLive && m_pos < m_line.size()
               && IsSpaceByte(m_line[m_pos]))
            ++m_pos;
        if (m_lineLive && m_pos < m_line.size())
            return InputReadStatus::Ok;
        std::string next;
        const InputReadStatus st = m_source.PullLine(next);
        if (st != InputReadStatus::Ok) return st;
        m_line = std::move(next);
        m_pos = 0;
        m_lineLive = true;
    }
}

} // namespace nlang
