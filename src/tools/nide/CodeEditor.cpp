/*--- CodeEditor.cpp - code text editor with a line-number area for NLang IDE ---*/
#include "CodeEditor.h"

#include "SyntaxHighlighter.h"


#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextStream>
#include <QWheelEvent>

namespace nlang {

namespace {
const int kTabStopWidthChars = 4;  // tab stop = 4 spaces
const int kLineAreaMarginPx = 8;   // padding on each side of line numbers
const int kBreakpointDotRadius = 4;  // gutter breakpoint dot
const int kStopArrowHeadPx = 5;    // gutter stopped-line arrow half-height
}

//--- LineArea ---

LineArea::LineArea(CodeEditor* editor)
    : QWidget(editor)
    , m_editor(editor)
{
}

QSize LineArea::sizeHint() const {
    return QSize(m_editor->lineAreaWidth(), 0);
}

void LineArea::paintEvent(QPaintEvent* event) {
    m_editor->paintLineArea(event);
}

void LineArea::mousePressEvent(QMouseEvent* event) {
    m_editor->handleGutterPress(event->pos());
}

//--- CodeEditor ---

CodeEditor::CodeEditor(QWidget* parent)
    : QPlainTextEdit(parent)
{
    m_lineArea = new LineArea(this);
    //Token coloring through the compiler's own lexer (CT_Editor mode).
    //The highlighter parents to the document and is never touched again.
    new SyntaxHighlighter(document());

    setFrameShape(QFrame::NoFrame);
    setTabStopDistance(kTabStopWidthChars * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    //Hover events without a held button: the Ctrl+click jump preview
    //(pointing-hand cursor) needs mouse moves while no button is down.
    setMouseTracking(true);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);

    updateLineAreaWidth(0);
    highlightCurrentLine();
}

CodeEditor::~CodeEditor() = default;

int CodeEditor::lineAreaWidth() const {
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }

    return kBreakpointColumnWidth + kLineAreaMarginPx * 2
        + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::paintBlockGutter(QPainter& painter, int blockNumber,
                                   int top) {
    const QString number = QString::number(blockNumber + 1);
    painter.setPen(Qt::darkCyan);
    painter.drawText(kBreakpointColumnWidth, top,
                     m_lineArea->width() - kBreakpointColumnWidth
                         - kLineAreaMarginPx,
                     fontMetrics().height(), Qt::AlignRight, number);

    const int centerY = top + fontMetrics().height() / 2;
    const int centerX = kBreakpointColumnWidth / 2;
    if (m_breakpointLines.contains(blockNumber + 1)) {
        //Bound in the live session = filled; unbound or no
        //session = hollow.
        if (m_boundBreakpointLines.contains(blockNumber + 1)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::red);
        } else {
            painter.setPen(QPen(Qt::red, 1));
            painter.setBrush(Qt::NoBrush);
        }
        painter.drawEllipse(QPoint(centerX, centerY),
                            kBreakpointDotRadius,
                            kBreakpointDotRadius);
    }
    if (m_stoppedLine == blockNumber + 1) {
        //The paused line's arrow; drawn over the dot when both
        //mark the same line.
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::red);
        static const QPointF arrow[] = {
            QPointF(2, centerY - kStopArrowHeadPx),
            QPointF(2, centerY + kStopArrowHeadPx),
            QPointF(kBreakpointColumnWidth - 2.0, centerY)};
        painter.drawPolygon(arrow, 3);
    }
}

void CodeEditor::paintLineArea(QPaintEvent* event) {
    QPainter painter(m_lineArea);
    painter.fillRect(event->rect(), QColor(Qt::lightGray).lighter(120));
    painter.setRenderHint(QPainter::Antialiasing);

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top())
            paintBlockGutter(painter, blockNumber, top);

        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void CodeEditor::setBreakpointLines(const QSet<int>& lines) {
    m_breakpointLines = lines;
    m_lineArea->update();
}

void CodeEditor::setBoundBreakpointLines(const QSet<int>& lines) {
    m_boundBreakpointLines = lines;
    m_lineArea->update();
}

void CodeEditor::setStoppedLine(int line) {
    m_stoppedLine = line;
    m_lineArea->update();
    highlightCurrentLine();
}

void CodeEditor::setEditorFontPt(int pointSize) {
    QFont sized = font();
    if (sized.pointSize() == pointSize)
        return;
    sized.setPointSize(pointSize);
    setFont(sized);
    //Font-derived geometry follows: tab stops, gutter width, gutter
    //layout.
    setTabStopDistance(kTabStopWidthChars
        * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    updateLineAreaWidth(0);
    layoutLineArea();
}

void CodeEditor::wheelEvent(QWheelEvent* event) {
    //Ctrl+wheel zooms the font. The size lives in the global settings,
    //so the editor only reports the notch direction; the owner applies
    //and persists the new size for every open editor.
    if (event->modifiers() & Qt::ControlModifier) {
        //High-resolution wheels report sub-notch deltas: accumulate
        //and emit one zoom point only per full ±120 notch (a zero
        //vertical delta adds nothing and no-ops).
        m_wheelZoomDelta += event->angleDelta().y();
        while (m_wheelZoomDelta >= 120 || m_wheelZoomDelta <= -120) {
            emit fontSizeZoomRequested(m_wheelZoomDelta > 0 ? 1 : -1);
            m_wheelZoomDelta += (m_wheelZoomDelta > 0) ? -120 : 120;
        }
        event->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(event);
}

void CodeEditor::handleGutterPress(const QPoint& pos) {
    //Only the breakpoint column toggles; presses on the line numbers
    //are ignored. Rejects clicks below the last line: cursorForPosition
    //snaps to the nearest block, which would toggle line 9 for a click
    //in the empty space under a 3-line file.
    if (pos.x() >= kBreakpointColumnWidth)
        return;
    const QTextBlock block =
        cursorForPosition(QPoint(0, pos.y())).block();
    if (!block.isValid())
        return;
    const QRectF geometry =
        blockBoundingGeometry(block).translated(contentOffset());
    if (pos.y() < geometry.top()
            || pos.y() >= geometry.top() + blockBoundingRect(block).height())
        return;
    emit breakpointToggled(block.blockNumber() + 1);
}

void CodeEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);

    layoutLineArea();
}

void CodeEditor::layoutLineArea() {
    const QRect cr = contentsRect();
    m_lineArea->setGeometry(
        QRect(cr.left(), cr.top(), lineAreaWidth(), cr.height()));
}

void CodeEditor::updateLineArea(const QRect& rect, int dy) {
    if (dy != 0)
        m_lineArea->scroll(0, dy);
    else
        m_lineArea->update(0, rect.y(), m_lineArea->width(), rect.height());

    //Whole-viewport invalidation without a block-count change (font
    //change etc.) must still refresh the gutter width.
    if (rect.contains(viewport()->rect()))
        updateLineAreaWidth(0);
}

void CodeEditor::updateLineAreaWidth(int newBlockCount) {
    Q_UNUSED(newBlockCount);
    setViewportMargins(lineAreaWidth(), 0, 0, 0);
}

void CodeEditor::highlightCurrentLine() {
    QList<QTextEdit::ExtraSelection> selections;

    //The debug-stop line is a SEPARATE selection set from the current
    //line: it survives cursor moves (which rebuild this list), so the
    //paused line stays marked while the user clicks around the stack.
    if (m_stoppedLine > 0) {
        const QTextBlock stopped = document()->findBlockByNumber(
            m_stoppedLine - 1);
        if (stopped.isValid()) {
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(QColor(Qt::red).lighter(176));
            selection.format.setProperty(QTextFormat::FullWidthSelection,
                                         true);
            selection.cursor = QTextCursor(stopped);
            selection.cursor.clearSelection();
            selections.append(selection);
        }
    }

    if (!isReadOnly()) {
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(QColor(Qt::green).lighter(192));
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        selections.append(selection);
    }

    setExtraSelections(selections);
}


} // namespace nlang
