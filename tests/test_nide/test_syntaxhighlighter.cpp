/*--- test_syntaxhighlighter.cpp - SyntaxHighlighter unit tests ---*/
#include "../../../src/tools/nide/SyntaxHighlighter.h"

//Token constants for the typeOf mapping tests.
#include "nlang.tab.h"

#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtTest>

using namespace nlang;

class TestSyntaxHighlighter : public QObject {
    Q_OBJECT

private:
    //Foreground color the highlighter applied at a block position;
    //invalid when the position has no format span.
    QColor colorAt(const QTextBlock& block, int pos) {
        for (const QTextLayout::FormatRange& range : block.layout()->formats()) {
            if (pos >= range.start && pos < range.start + range.length)
                return range.format.foreground().color();
        }
        return QColor();
    }

private slots:
    // --- pure mapping ---

    void testTypeOfMapping() {
        QCOMPARE(SyntaxHighlighter::typeOf(TT_Int), SyntaxHighlighter::HTT_Number);
        QCOMPARE(SyntaxHighlighter::typeOf(TT_Byte), SyntaxHighlighter::HTT_Number);
        QCOMPARE(SyntaxHighlighter::typeOf(TT_Float), SyntaxHighlighter::HTT_Number);
        QCOMPARE(SyntaxHighlighter::typeOf(KT_Bool), SyntaxHighlighter::HTT_Keyword);
        QCOMPARE(SyntaxHighlighter::typeOf(KT_While), SyntaxHighlighter::HTT_Keyword);
        QCOMPARE(SyntaxHighlighter::typeOf(TT_Char), SyntaxHighlighter::HTT_Char);
        QCOMPARE(SyntaxHighlighter::typeOf(TT_String), SyntaxHighlighter::HTT_String);
        QCOMPARE(SyntaxHighlighter::typeOf(TT_Comment), SyntaxHighlighter::HTT_Comment);
        QCOMPARE(SyntaxHighlighter::typeOf(TT_Error), SyntaxHighlighter::HTT_Error);
        //Operators and punctuation stay default.
        QCOMPARE(SyntaxHighlighter::typeOf(OT_EQ), SyntaxHighlighter::HTT_Default);
        QCOMPARE(SyntaxHighlighter::typeOf(P_Then), SyntaxHighlighter::HTT_Default);
        QCOMPARE(SyntaxHighlighter::typeOf(';'), SyntaxHighlighter::HTT_Default);
    }

    // --- single-block coloring ---

    void testKeywordHighlighted() {
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("if (x) return;");
        highlighter.rehighlight();
        QTextBlock block = doc.firstBlock();
        QCOMPARE(colorAt(block, 0), QColor(Qt::blue));      // "if"
        QCOMPARE(colorAt(block, 7), QColor(Qt::blue));      // "return"
        QCOMPARE(colorAt(block, 4), QColor(Qt::black));     // identifier "x"
    }

    void testNumberHighlighted() {
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("x = 42;");
        highlighter.rehighlight();
        QCOMPARE(colorAt(doc.firstBlock(), 4), QColor(Qt::darkCyan));
    }

    void testStringHighlighted() {
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("s = \"hi\";");
        highlighter.rehighlight();
        QTextBlock block = doc.firstBlock();
        QCOMPARE(colorAt(block, 4), QColor(Qt::darkRed));   // opening quote
        QCOMPARE(colorAt(block, 7), QColor(Qt::darkRed));   // closing quote
    }

    void testCharLiteralHighlighted() {
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("c = 'a';");
        highlighter.rehighlight();
        QCOMPARE(colorAt(doc.firstBlock(), 4), QColor(Qt::darkRed));
    }

    void testLineCommentHighlighted() {
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("int x; // note");
        highlighter.rehighlight();
        QTextBlock block = doc.firstBlock();
        QCOMPARE(colorAt(block, 7), QColor(Qt::darkGreen));   // "//"
        QCOMPARE(colorAt(block, 13), QColor(Qt::darkGreen));  // comment tail
        QCOMPARE(colorAt(block, 0), QColor(Qt::blue));        // "int" still keyword
    }

    void testUnterminatedStringIsError() {
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("s = \"abc");
        highlighter.rehighlight();
        QTextBlock block = doc.firstBlock();
        QCOMPARE(colorAt(block, 4), QColor(Qt::red));   // error red, not string darkRed
        QCOMPARE(colorAt(block, 7), QColor(Qt::red));
    }

    void testDefaultSpanIsBlack() {
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("alpha");
        highlighter.rehighlight();
        QCOMPARE(colorAt(doc.firstBlock(), 0), QColor(Qt::black));
    }

    // --- block comment across blocks (state carry) ---

    void testBlockCommentSpansBlocks() {
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("int a;\n/* still\nopen*/ int b;");
        highlighter.rehighlight();

        QTextBlock b1 = doc.firstBlock().next();   // "/* still"
        QTextBlock b2 = b1.next();                 // "open*/ int b;"

        QCOMPARE(colorAt(b1, 0), QColor(Qt::darkGreen));   // from the "/*"
        QCOMPARE(colorAt(b1, 7), QColor(Qt::darkGreen));   // block tail
        QVERIFY(b1.userState() != 0);                      // state carried

        QCOMPARE(colorAt(b2, 0), QColor(Qt::darkGreen));   // resumed comment
        QCOMPARE(colorAt(b2, 5), QColor(Qt::darkGreen));   // "*/" close
        QCOMPARE(colorAt(b2, 7), QColor(Qt::blue));        // "int" keyword after
        QCOMPARE(colorAt(b2, 11), QColor(Qt::black));      // identifier "b"
        QCOMPARE(b2.userState(), 0);                       // back to INITIAL
    }

    void testEmptyBlockInsideCommentKeepsState() {
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("/* a\n\nb */");
        highlighter.rehighlight();

        QTextBlock b1 = doc.firstBlock().next();   // empty middle block
        QTextBlock b2 = b1.next();                 // "b */"
        QVERIFY(b1.userState() != 0);              // still inside the comment
        QCOMPARE(colorAt(b2, 2), QColor(Qt::darkGreen));   // "*/" still comment
        QCOMPARE(b2.userState(), 0);
    }

    void testBlockCommentMiddleBlockHighlighted() {
        //A NON-empty middle block of a 3-block comment must be fully
        //green: the mid-comment body match restarts the column counter,
        //so the deferred token spans the whole block.
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("/* a\nstill open\nb */ int c;");
        highlighter.rehighlight();

        QTextBlock b1 = doc.firstBlock().next();   // "still open"
        QTextBlock b2 = b1.next();                 // "b */ int c;"
        QCOMPARE(colorAt(b1, 0), QColor(Qt::darkGreen));   // whole mid block
        QCOMPARE(colorAt(b1, 9), QColor(Qt::darkGreen));   // last char
        QVERIFY(b1.userState() != 0);
        QCOMPARE(colorAt(b2, 0), QColor(Qt::darkGreen));   // "b */"
        QCOMPARE(colorAt(b2, 5), QColor(Qt::blue));        // "int" after close
        QCOMPARE(colorAt(b2, 9), QColor(Qt::black));       // identifier "c"
    }

    void testUnterminatedBlockCommentAtDocumentEnd() {
        //The comment reaches EOF still open: the tail colors green and
        //the block state stays non-zero (a later appended block would
        //resume COMMENT_S).
        QTextDocument doc;
        SyntaxHighlighter highlighter(&doc);
        doc.setPlainText("int a; /* tail");
        highlighter.rehighlight();

        QTextBlock b0 = doc.firstBlock();
        QCOMPARE(colorAt(b0, 0), QColor(Qt::blue));         // "int"
        QCOMPARE(colorAt(b0, 4), QColor(Qt::black));       // identifier "a"
        QCOMPARE(colorAt(b0, 7), QColor(Qt::darkGreen));   // "/*" starts
        QCOMPARE(colorAt(b0, 13), QColor(Qt::darkGreen));  // last char
        QVERIFY(b0.userState() != 0);                      // still open
    }
};

QTEST_MAIN(TestSyntaxHighlighter)
#include "test_syntaxhighlighter.moc"
