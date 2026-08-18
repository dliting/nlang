/*--- SyntaxHighlighter.cpp - NLang token coloring for the code editor ---*/
#include "SyntaxHighlighter.h"

#include "nlang.tab.h"

#include <QColor>

namespace nlang {

namespace {
const QColor kTokenColors[SyntaxHighlighter::HTT_COUNT] = {
    Qt::black,      // HTT_Default
    Qt::darkCyan,   // HTT_Number
    Qt::darkRed,    // HTT_Char
    Qt::darkRed,    // HTT_String
    Qt::blue,       // HTT_Keyword
    Qt::darkGreen,  // HTT_Comment
    Qt::red,        // HTT_Error
};
}

SyntaxHighlighter::SyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
    , m_scanner(ScriptScanner::CT_Editor)
{
    for (int i = 0; i < HTT_COUNT; ++i)
        m_formats[i].setForeground(kTokenColors[i]);
}

SyntaxHighlighter::HighlightType SyntaxHighlighter::typeOf(int tokenType) {
    //Number and keyword tokens form contiguous ranges in nlang.tab.h;
    //match the ranges, not each value.
    if (tokenType >= TT_Byte && tokenType <= TT_Float)
        return HTT_Number;
    if (tokenType >= KT_Bool && tokenType <= KT_While)
        return HTT_Keyword;
    switch (tokenType) {
    case TT_Char: return HTT_Char;
    case TT_String: return HTT_String;
    case TT_Comment: return HTT_Comment;
    case TT_Error: return HTT_Error;
    default: return HTT_Default;
    }
}

void SyntaxHighlighter::highlightBlock(const QString& text) {
    const QByteArray latin1 = text.toLatin1();
    m_scanner.OpenString(latin1.constData());

    //yylex resumes from StartState on its first call (nlang.l begins
    //with BEGIN(scanner.StartState())), so restoring the previous
    //block's state continues a block comment mid-text; 0 is INITIAL.
    int prevState = previousBlockState();
    if (prevState < 0)
        prevState = 0;
    m_scanner.StartState(prevState);

    int tokenType;
    while ((tokenType = m_scanner.NextToken()) > 0) {
        const ScriptLocation& loc = m_scanner.Location();
        int start = static_cast<int>(loc.m_nStartCol) - 1;
        int length = static_cast<int>(loc.m_nEndCol - loc.m_nStartCol) + 1;
        //An empty block inside a comment reports a zero-length span.
        if (start < 0 || length <= 0)
            continue;
        setFormat(start, length, m_formats[typeOf(tokenType)]);
    }
    m_scanner.CloseString();

    //COMMENT_S survives to the next block; everything else reports 0.
    setCurrentBlockState(m_scanner.StartState());
}

} // namespace nlang
