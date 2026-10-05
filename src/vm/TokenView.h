/*---
TokenView.h — the input cursor behind io's readLine/readToken/readChar/
hasInput: one shared line-remainder buffer so mixed reads compose with
C++ cin>>/getline semantics (a token read leaves the line remainder for
the next readLine). Works over any LineSource; end-of-input follows the
C/C++/Java split the manual documents — line-level consumption treats
it as a normal "" sentinel, demanding a token/char there raises.
---*/
#pragma once
#include <cstdint>
#include <string>

#include "LineSource.h"

namespace nlang {

class TokenView {
public:
    explicit TokenView(LineSource& source) : m_source(source) {}

    //Remainder of the current line; pulls a fresh line when the current
    //one is exhausted. Eof answers with an empty outLine (the ""
    //sentinel — line-level reads never raise; the callback folds it).
    InputReadStatus ReadLine(std::string& outLine);
    //Skip whitespace (crossing lines as needed), then one whitespace-
    //free run. Tokens never span lines: the line break itself is
    //whitespace.
    InputReadStatus ReadToken(std::string& outToken);
    //Skip whitespace, then decode one full UTF-8 scalar value. A scalar
    //can never contain a newline byte, so a truncated sequence at the
    //line end is genuinely invalid input, not a split encoding.
    //invalidUtf8 is meaningful only when the status is Ok.
    InputReadStatus ReadChar(uint32_t& outChar, bool& invalidUtf8);
    //Non-consuming probe, never raises: a live current line (even an
    //empty remainder answers true — one more "" readLine before the
    //end) or more bytes from the source.
    bool HasInput();

private:
    //Advance past whitespace, pulling lines as needed. Ok = cursor sits
    //on a visible non-whitespace byte.
    InputReadStatus SkipWhitespace();

    LineSource& m_source;
    std::string m_line;       //current line, newline already stripped
    size_t m_pos = 0;         //cursor into m_line
    bool m_lineLive = false;  //m_line holds an unconsumed remainder
};

} // namespace nlang
