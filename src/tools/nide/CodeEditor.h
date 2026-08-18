/*--- CodeEditor.h - code text editor with a line-number area for NLang IDE ---*/
#ifndef NLANG_TOOLS_NIDE_CODE_EDITOR_H
#define NLANG_TOOLS_NIDE_CODE_EDITOR_H

#include "FileEditor.h"

#include <QPlainTextEdit>

namespace nlang {

class CodeEditor;

//--- LineArea: the line-number gutter, painted by its CodeEditor.
class LineArea : public QWidget {
public:
    explicit LineArea(CodeEditor* editor);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    CodeEditor* m_editor;  // non-owning back-pointer
};

//--- CodeEditor: QPlainTextEdit with line numbers and a current-line
//  highlight. Syntax highlighting is attached in a later step.
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeEditor(QWidget* parent = nullptr);
    ~CodeEditor() override;

    int lineAreaWidth() const;

    //The line-number gutter widget (child of the editor).
    LineArea* lineArea() const { return m_lineArea; }

    //Paints the line numbers (called by LineArea::paintEvent).
    void paintLineArea(QPaintEvent* event);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateLineArea(const QRect& rect, int dy);
    void updateLineAreaWidth(int newBlockCount);
    void highlightCurrentLine();

private:
    LineArea* m_lineArea;
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
