/*--- CodeEditor.cpp - code text editor with a line-number area for NLang IDE ---*/
#include "CodeEditor.h"

#include "SyntaxHighlighter.h"

#include "nlang/langservice/SymbolIndex.h"

#include <QCoreApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QListWidget>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextStream>
#include <QToolTip>

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

    QRect cr = contentsRect();
    m_lineArea->setGeometry(QRect(cr.left(), cr.top(), lineAreaWidth(), cr.height()));
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


//--- Library-backed code assistance (signature help / completion / F12) ---

void CodeEditor::setSymbolIndex(const langservice::SymbolIndex* index) {
    m_symbolIndex = index;
}

namespace {
//Walk left from `chainEnd` (a position on the '.' before the call name)
//over ident ('.' ident)* package segments; returns the chain's first
//character index. Identifiers are ASCII word characters (the lexer's
//keyword/library alphabet).
int PackageChainStart(const QString& lineText, int chainEnd) {
    auto isIdent = [](QChar ch) {
        return ch.isLetterOrNumber() || ch == '_';
    };
    int chainStart = chainEnd;
    while (chainStart > 1 && lineText.at(chainStart - 1) == QLatin1Char('.')
        && lineText.at(chainStart - 2).isLetterOrNumber())
    {
        int segEnd = chainStart - 1;
        int segStart = segEnd;
        while (segStart > 0 && isIdent(lineText.at(segStart - 1)))
            --segStart;
        if (segStart == segEnd)
            break;
        chainStart = segStart;
    }
    return chainStart;
}
} // namespace

QString CodeEditor::qualifiedNameAt(const QString& lineText, int column) {
    // Identifier characters nlang uses (ASCII word chars; nlang is ASCII
    // for keywords and library names).
    auto isIdent = [](QChar ch) {
        return ch.isLetterOrNumber() || ch == '_';
    };

    // The right identifier (the call name) either contains the column or
    // sits immediately to its left.
    int end = column;
    while (end < lineText.length() && isIdent(lineText.at(end)))
        ++end;
    int nameStart = end;
    while (nameStart > 0 && isIdent(lineText.at(nameStart - 1)))
        --nameStart;

    // When the column is in whitespace or punctuation and not adjacent to
    // an identifier, there is no name under the cursor.
    if (nameStart == end)
        return QString();
    // Column must touch this identifier (inside it, or right after it).
    if (column < nameStart || column > end)
        return QString();

    // Look left for '.' then another identifier.
    int p = nameStart;
    if (p <= 0 || lineText.at(p - 1) != QLatin1Char('.'))
        return QString();
    int nsEnd = p - 1;
    int nsStart = nsEnd;
    while (nsStart > 0 && isIdent(lineText.at(nsStart - 1)))
        --nsStart;
    if (nsStart == nsEnd)
        return QString();
    // Extend left across a dotted PACKAGE prefix: library calls are
    // package-qualified (`io.print`, `vendor.graphics.hue`), so the whole
    // chain left of the call name is the package path. An object member
    // chain yields the same shape; the index lookup below simply misses
    // for those.
    const int chainStart = PackageChainStart(lineText, nsStart);

    return lineText.mid(chainStart, p - chainStart)
         + lineText.mid(nameStart, end - nameStart);
}

QString CodeEditor::formatSymbol(const langservice::SymbolInfo& symbol) {
    QString text;
    if (symbol.native)
        text += QObject::tr("[native]") + QLatin1Char('\n');
    QString params;
    for (size_t i = 0; i < symbol.params.size(); ++i) {
        if (i)
            params += QStringLiteral(", ");
        params += QString::fromStdString(symbol.params[i].type)
                + QLatin1Char(' ')
                + QString::fromStdString(symbol.params[i].name);
    }
    text += QString::fromStdString(symbol.returnType) + QLatin1Char(' ')
          + QString::fromStdString(symbol.pkg) + QLatin1Char('.')
          + QString::fromStdString(symbol.name)
          + QLatin1Char('(') + params + QStringLiteral(")");
    if (!symbol.doc.empty()) {
        text += QLatin1Char('\n');
        for (const std::string& line : symbol.doc)
            text += QLatin1Char('\n') + QString::fromStdString(line);
    }
    return text;
}

bool CodeEditor::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip)
        return handleToolTip(static_cast<QHelpEvent*>(event));
    return QPlainTextEdit::event(event);
}

bool CodeEditor::handleToolTip(QHelpEvent* helpEvent) {
    if (!m_symbolIndex) {
        QToolTip::hideText();
        return false;
    }
    const QTextCursor cursor = cursorForPosition(helpEvent->pos());
    const QString qualified =
        qualifiedNameAt(cursor.block().text(), cursor.positionInBlock());
    if (qualified.contains(QLatin1Char('.'))) {
        const QStringList parts = qualified.split(QLatin1Char('.'));
        const langservice::SymbolInfo* symbol =
            m_symbolIndex->Resolve(parts[0].toStdString(),
                                   parts[1].toStdString());
        if (symbol) {
            QToolTip::showText(helpEvent->globalPos(),
                               formatSymbol(*symbol), this);
            return true;
        }
    }
    QToolTip::hideText();
    return false;
}

void CodeEditor::keyPressEvent(QKeyEvent* event) {
    if (m_completionPopup && m_completionPopup->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Escape:
            closeCompletion();
            return;
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
            QCoreApplication::sendEvent(m_completionPopup, event);
            return;
        default:
            closeCompletion();
            break;
        }
    }

    if (event->key() == Qt::Key_F12 && m_symbolIndex) {
        const QTextBlock block = textCursor().block();
        const QString qualified =
            qualifiedNameAt(block.text(), textCursor().positionInBlock());
        if (qualified.contains(QLatin1Char('.'))) {
            const QStringList parts = qualified.split(QLatin1Char('.'));
            const langservice::SymbolInfo* symbol =
                m_symbolIndex->Resolve(parts[0].toStdString(),
                                       parts[1].toStdString());
            if (symbol) {
                emit goToDefinitionRequested(
                    QString::fromStdString(symbol->filePath), symbol->line);
                return;
            }
        }
    }

    QPlainTextEdit::keyPressEvent(event);

    if (event->key() == Qt::Key_Period)
        triggerPackageCompletion();
}

QString CodeEditor::packageTokenBeforeDot() {
    // The dotted package chain left of the just-typed '.' (`vendor.`
    // collects `vendor`; `vendor.graphics.` collects `vendor.graphics`).
    const QTextBlock block = textCursor().block();
    const QString text = block.text();
    const int dot = textCursor().positionInBlock() - 1;  // '.' position
    int start = dot;
    for (;;) {
        while (start > 0) {
            const QChar ch = text.at(start - 1);
            if (!ch.isLetterOrNumber() && ch != '_')
                break;
            --start;
        }
        if (start > 1 && text.at(start - 1) == QLatin1Char('.'))
            --start;   // consume the dot, scan the previous segment
        else
            break;
    }
    if (start == dot)
        return QString();
    return text.mid(start, dot - start);
}

void CodeEditor::showCompletionPopup(
    const std::vector<const langservice::SymbolInfo*>& candidates) {
    closeCompletion();
    m_completionPopup = new QListWidget(this);
    m_completionPopup->setWindowFlags(Qt::ToolTip | Qt::WindowStaysOnTopHint);
    m_completionPopup->setFocusPolicy(Qt::NoFocus);
    for (const langservice::SymbolInfo* symbol : candidates) {
        auto* item = new QListWidgetItem(
            QString::fromStdString(symbol->name), m_completionPopup);
        //Stash the full qualified name for insertion.
        item->setData(Qt::UserRole,
                      QString::fromStdString(symbol->pkg) + QLatin1Char('.')
                      + QString::fromStdString(symbol->name));
    }
    connect(m_completionPopup, &QListWidget::itemClicked,
            this, &CodeEditor::applyCompletion);
    connect(m_completionPopup, &QListWidget::itemActivated,
            this, &CodeEditor::applyCompletion);

    //Position the popup at the cursor, on the text viewport.
    QPoint pos = viewport()->mapToGlobal(cursorRect().bottomLeft());
    m_completionPopup->move(pos.x(), pos.y() + 2);
    m_completionPopup->resize(260,
        std::min(180, 18 * static_cast<int>(candidates.size()) + 6));
    m_completionPopup->setCurrentRow(0);
    m_completionPopup->show();
}

void CodeEditor::triggerPackageCompletion() {
    if (!m_symbolIndex)
        return;
    const QString pkg = packageTokenBeforeDot();
    if (pkg.isEmpty())
        return;
    //The chain left of the just-typed '.' must be a known package.
    const auto candidates =
        m_symbolIndex->CompletePackage(pkg.toStdString());
    if (!candidates.empty())
        showCompletionPopup(candidates);
}

void CodeEditor::applyCompletion(QListWidgetItem* item) {
    if (!item)
        return;
    // Insert just the function name (the package prefix is already typed).
    insertPlainText(QString::fromStdString(
        item->data(Qt::UserRole).toString().section(
            QLatin1Char('.'), -1).toStdString()));
    closeCompletion();
}

void CodeEditor::closeCompletion() {
    if (m_completionPopup) {
        m_completionPopup->hide();
        m_completionPopup->deleteLater();
        m_completionPopup = nullptr;
    }
}

} // namespace nlang
