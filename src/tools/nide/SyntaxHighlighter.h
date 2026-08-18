/*--- SyntaxHighlighter.h - NLang token coloring for the code editor ---*/
#ifndef NLANG_TOOLS_NIDE_SYNTAX_HIGHLIGHTER_H
#define NLANG_TOOLS_NIDE_SYNTAX_HIGHLIGHTER_H

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <nlang/compiler/ScriptScanner.h>

namespace nlang {

//--- SyntaxHighlighter: colors one text block by scanning it with the
//  compiler's own lexer (CT_Editor mode), so the editor and the
//  compiler always agree on what a token is. A block comment spanning
//  blocks is carried through the Qt block state (scanner start state).
//  Columns are byte columns of the Latin-1 conversion -- toLatin1 maps
//  every character to exactly one byte (others become '?'), so spans
//  always line up with QString positions; the only degradation is that
//  non-ASCII characters fall out of token recognition (default color).
class SyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    //Highlight categories; order fixes the color table.
    enum HighlightType {
        HTT_Default,
        HTT_Number,
        HTT_Char,
        HTT_String,
        HTT_Keyword,
        HTT_Comment,
        HTT_Error,
        HTT_COUNT
    };

    explicit SyntaxHighlighter(QTextDocument* parent);

    //Token type (yytokentype from nlang.tab.h) -> highlight category.
    static HighlightType typeOf(int tokenType);

protected:
    void highlightBlock(const QString& text) override;

private:
    ScriptScanner m_scanner;
    QTextCharFormat m_formats[HTT_COUNT];
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_SYNTAX_HIGHLIGHTER_H
