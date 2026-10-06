/*--- CodeEditor.h - code text editor with a line-number area for NLang IDE ---*/
#ifndef NLANG_TOOLS_NIDE_CODE_EDITOR_H
#define NLANG_TOOLS_NIDE_CODE_EDITOR_H

#include "FileEditor.h"

#include <QHelpEvent>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSet>

#include <vector>

namespace nlang {

class CodeEditor;

namespace langservice {
class SymbolIndex;
struct SymbolInfo;
}

//--- LineArea: the line-number gutter, painted by its CodeEditor.
class LineArea : public QWidget {
public:
    explicit LineArea(CodeEditor* editor);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    CodeEditor* m_editor;  // non-owning back-pointer
};

//--- CodeEditor: QPlainTextEdit with line numbers, a breakpoint gutter
//  column, a current-line highlight, a debug-stop marker, and token
//  coloring (SyntaxHighlighter, attached in the ctor).
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    //Width of the leading breakpoint column of the gutter.
    static const int kBreakpointColumnWidth = 16;

    explicit CodeEditor(QWidget* parent = nullptr);
    ~CodeEditor() override;

    int lineAreaWidth() const;

    //The line-number gutter widget (child of the editor).
    LineArea* lineArea() const { return m_lineArea; }

    //Paints the gutter (called by LineArea::paintEvent).
    void paintLineArea(QPaintEvent* event);

    //Debug surface fed by MainWindow: which lines carry breakpoints and
    //which of them are bound in the live debug session (filled vs hollow
    //dots), plus the paused line (0 = none).
    void setBreakpointLines(const QSet<int>& lines);
    void setBoundBreakpointLines(const QSet<int>& lines);
    void setStoppedLine(int line);
    const QSet<int>& breakpointLines() const { return m_breakpointLines; }
    const QSet<int>& boundBreakpointLines() const
    {
        return m_boundBreakpointLines;
    }
    int stoppedLine() const { return m_stoppedLine; }

    //Handle a press inside the gutter (mapped by LineArea): a hit in the
    //breakpoint column toggles that line's breakpoint.
    void handleGutterPress(const QPoint& pos);

    //--- Library-backed code assistance (signature help / completion /
    //    go-to-definition). The index is owned by the MainWindow and may
    //    be null (assistance then stays dormant).
    void setSymbolIndex(const langservice::SymbolIndex* index);

    //Pure text helpers, exposed for unit testing:
    // Extract the qualified name ("ns.name") under a 0-based column of a
    // single line, or "" when the cursor is not on ns.name.
    static QString qualifiedNameAt(const QString& lineText, int column);
    // Build the hover text for a library symbol.
    static QString formatSymbol(const langservice::SymbolInfo& symbol);

protected:
    void resizeEvent(QResizeEvent* event) override;
    //Menu-like dismissal for the completion popup: any cursor-replacing
    //click in the editor and any focus loss close it.
    void mousePressEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void updateLineArea(const QRect& rect, int dy);
    void updateLineAreaWidth(int newBlockCount);
    void highlightCurrentLine();

private:
    //One gutter row of paintLineArea: the line number, the breakpoint
    //dot (filled when bound in the live session, hollow otherwise) and
    //the paused-line arrow (drawn over the dot when both mark the same
    //line).
    void paintBlockGutter(QPainter& painter, int blockNumber, int top);

    //Signature help tooltip at the cursor under the mouse.
    bool handleToolTip(QHelpEvent* helpEvent);
    //Open a completion popup right after a typed '.' when the token to
    //the left is a known namespace.
    void triggerPackageCompletion();
    //Namespace token immediately left of the just-typed '.', or "".
    QString packageTokenBeforeDot();
    //Build, fill and position the completion popup for candidates.
    void showCompletionPopup(
        const std::vector<const langservice::SymbolInfo*>& candidates);
    void applyCompletion(QListWidgetItem* item);
    void closeCompletion();

    LineArea* m_lineArea;
    QSet<int> m_breakpointLines;
    QSet<int> m_boundBreakpointLines;
    int m_stoppedLine = 0;

    const langservice::SymbolIndex* m_symbolIndex = nullptr;
    QListWidget* m_completionPopup = nullptr;

signals:
    //A gutter click toggled the breakpoint of this 1-based line; the
    //owner resolves the file (the editor itself stays path-free).
    void breakpointToggled(int line);
    //F12 on a library symbol: the owner opens filePath at line.
    void goToDefinitionRequested(const QString& filePath, int line);
};

//--- CodeFileEditor: a source file bound to a CodeEditor.
class CodeFileEditor : public FileEditor {
    Q_OBJECT

public:
    CodeFileEditor(EditorManager& owner, const QString& absoluteFilePath);

    QWidget* widget() override;
    QString positionInfo() override;

protected:
    bool doSave() override;
    bool doOpen() override;
    bool doCreate() override;

private:
    CodeEditor m_editor;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_CODE_EDITOR_H
