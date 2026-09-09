/*--- CodeEditor.h - code text editor with a line-number area for NLang IDE ---*/
#ifndef NLANG_TOOLS_NIDE_CODE_EDITOR_H
#define NLANG_TOOLS_NIDE_CODE_EDITOR_H

#include "FileEditor.h"

#include <QPlainTextEdit>
#include <QSet>

namespace nlang {

class CodeEditor;

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
    int stoppedLine() const { return m_stoppedLine; }

    //Handle a press inside the gutter (mapped by LineArea): a hit in the
    //breakpoint column toggles that line's breakpoint.
    void handleGutterPress(const QPoint& pos);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateLineArea(const QRect& rect, int dy);
    void updateLineAreaWidth(int newBlockCount);
    void highlightCurrentLine();

private:
    LineArea* m_lineArea;
    QSet<int> m_breakpointLines;
    QSet<int> m_boundBreakpointLines;
    int m_stoppedLine = 0;

signals:
    //A gutter click toggled the breakpoint of this 1-based line; the
    //owner resolves the file (the editor itself stays path-free).
    void breakpointToggled(int line);
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
