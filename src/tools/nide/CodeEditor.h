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

    //--- Font: one shared size for every code editor, owned by the
    //    MainWindow's settings (Tools > Options and Ctrl+wheel write it).
    void setEditorFontPt(int pointSize);

    //Pure text helpers, exposed for unit testing:
    // Extract the qualified name ("ns.name") under a 0-based column of a
    // single line, or "" when the cursor is not on ns.name.
    static QString qualifiedNameAt(const QString& lineText, int column);
    // Build the hover text for a library symbol.
    static QString formatSymbol(const langservice::SymbolInfo& symbol);

protected:
    void resizeEvent(QResizeEvent* event) override;
    //Menu-like dismissal for the completion popup: any cursor-replacing
    //click in the editor and any focus loss close it. A Ctrl+click on a
    //resolvable name goes to the definition instead.
    void mousePressEvent(QMouseEvent* event) override;
    //Pointing-hand affordance over resolvable names while Ctrl is held
    //(the Ctrl+click jump preview).
    void mouseMoveEvent(QMouseEvent* event) override;
    //Ctrl+wheel zooms the editor font (the size lives in the global
    //settings; the owner applies it to every editor and persists it).
    void wheelEvent(QWheelEvent* event) override;
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
    //Re-place the gutter widget over the viewport's left margin (resize
    //and font-size changes both reflow it).
    void layoutLineArea();

    //Signature help tooltip at the cursor under the mouse.
    bool handleToolTip(QHelpEvent* helpEvent);
    //Resolve the qualified name ("pkg.name" / "vendor.pkg.name") at the
    //cursor against the symbol index; null when nothing resolves. The
    //package is the whole dotted prefix, the name its last segment.
    const langservice::SymbolInfo* resolveAt(
        const QTextCursor& cursor) const;
    //Go to the definition at the cursor (F12/F6/Ctrl+click); true when
    //a symbol resolved and the request was emitted.
    bool goToDefinitionAt(const QTextCursor& cursor);
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
    //Ctrl+wheel zoom accumulator: high-resolution wheels report
    //fractions of a notch; only a full ±120 zooms one point.
    int m_wheelZoomDelta = 0;

    const langservice::SymbolIndex* m_symbolIndex = nullptr;
    QListWidget* m_completionPopup = nullptr;

signals:
    //A gutter click toggled the breakpoint of this 1-based line; the
    //owner resolves the file (the editor itself stays path-free).
    void breakpointToggled(int line);
    //Go-to-definition (F12, F6 or Ctrl+click) hit a symbol: the owner
    //opens filePath at line.
    void goToDefinitionRequested(const QString& filePath, int line);
    //One Ctrl+wheel notch: +1 zooms in, -1 zooms out. The owner applies
    //the new size to every editor and persists it.
    void fontSizeZoomRequested(int direction);
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
