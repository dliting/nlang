/*---
TerminalWidget.cpp — state and non-paint behaviour of the terminal
viewport: grid relayout, damage-driven repaint scheduling, selection,
wheel scrollback, cursor blink. The paint pass lives in
TerminalWidgetPaint.cpp.
---*/
#include "TerminalWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QFontMetrics>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <vector>

namespace nlang {
namespace terminal {
namespace {

constexpr int kCursorBlinkIntervalMs = 530;
constexpr int kWheelLinesPerNotch = 3;

} // namespace

TerminalWidget::TerminalWidget(QWidget* parent) : QWidget(parent) {
    setObjectName("terminalWidget");
    QFont base("Consolas");
    base.setStyleHint(QFont::Monospace);   //fallback chain off-Windows
    setFont(base);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    m_boldFont = base;
    m_boldFont.setBold(true);
    m_pBlinkTimer = new QTimer(this);
    m_pBlinkTimer->setInterval(kCursorBlinkIntervalMs);
    connect(m_pBlinkTimer, &QTimer::timeout, this,
            &TerminalWidget::blinkCursor);
    m_pBlinkTimer->start();
    updateCellMetrics();   //metrics only: the grid keeps the emulator's
                           //default until real on-screen geometry arrives
}

QSize TerminalWidget::sizeHint() const {
    //Derived from the CURRENT grid (pre-show: the emulator's default,
    //wherever a future setting moves it — never a second hardcoded copy).
    return QSize(kCellPadding * 2 + m_emulator.columns() * m_cellWidth,
                 kCellPadding * 2 + m_emulator.rows() * m_cellHeight);
}

void TerminalWidget::feedBytes(const QByteArray& vtBytes) {
    //Output can arrive while this page is hidden (a debug session feeds
    //the shared terminal from the debug tab) and no resize event will
    //follow — the page's geometry was fixed long before. Adopt it now,
    //before the text lands: fed on a stale grid, the text rides the
    //first-show relayout into the scrollback. relayoutGrid itself gates
    //on real geometry, so this is a no-op while the window is hidden.
    relayoutGrid();
    m_emulator.Feed(std::string(vtBytes.constData(),
                                static_cast<std::size_t>(vtBytes.size())));
    repaintDamaged();
}

void TerminalWidget::feedUtf8(const QString& text) {
    //Cooked-text channel: its '\n' means "this line is done" (the debug
    //wire even delivers io.print's newline as its own event), but a
    //terminal newline is CRLF — libvterm's bare LF only moves down and
    //the column survives every newline, so the text staircases right.
    //Normalize every bare LF to CRLF; an explicit CRLF passes through as
    //one break. Raw pty bytes use feedBytes and stay untouched.
    QString cooked;
    cooked.reserve(text.size() + text.count(QLatin1Char('\n')));
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\n')
                && (i == 0 || text.at(i - 1) != QLatin1Char('\r'))) {
            cooked += QLatin1Char('\r');
            cooked += QLatin1Char('\n');
        } else {
            cooked += c;
        }
    }
    feedBytes(cooked.toUtf8());
}

void TerminalWidget::resetTerminal() {
    m_emulator.Reset();
    m_scrollOffset = 0;
    clearSelection();
    repaintDamaged();   //Reset() reports full damage + cursor move
}

void TerminalWidget::updateCellMetrics() {
    const QFontMetrics metrics(font());
    m_cellWidth = std::max(1, metrics.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = std::max(1, metrics.lineSpacing());
    m_boldFont = font();
    m_boldFont.setBold(true);
}

void TerminalWidget::relayoutGrid() {
    //Adopt geometry only once it is real: while the window is hidden,
    //resizes carry the layout's placeholder size, which says nothing
    //about the terminal the user will see. Once the window is visible,
    //a hidden page must be synced to the rect its stack assigns before
    //its size is trusted (see syncHiddenPageGeometry).
    const QWidget* root = window();
    if (root == nullptr || !root->isVisible())
        return;
    if (!isVisible())
        syncHiddenPageGeometry();
    updateCellMetrics();
    const int newColumns = std::max(
        kMinColumns, (width() - 2 * kCellPadding) / m_cellWidth);
    const int newRows = std::max(
        kMinRows, (height() - 2 * kCellPadding) / m_cellHeight);
    if (newColumns != m_emulator.columns()
        || newRows != m_emulator.rows()) {
        m_emulator.Resize(newColumns, newRows);
        emit sizeChanged(newColumns, newRows);
    }
}

void TerminalWidget::syncHiddenPageGeometry() {
    //QStackedLayout (StackOne) assigns geometry to the CURRENT page
    //only; a page that was never current keeps its constructed
    //placeholder size (typically 640x480), and output fed on that
    //placeholder rides the first-show relayout into the scrollback.
    //The current page's geometry IS the rect the stack hands to every
    //page: QStackedLayout::setGeometry passes the stack's client rect
    //straight to currentWidget() and never stores it in the QLayout
    //geometry channel, so layout->contentsRect() is PERMANENTLY
    //meaningless for a stack — the assigned widget rect is the only
    //truth (Qt's own setStackingMode reads it the same way).
    std::vector<QWidget*> chain{this};   //this … up to the page
    QStackedWidget* stack = nullptr;
    for (QWidget* ancestor = parentWidget(); ancestor != nullptr;
         ancestor = ancestor->parentWidget()) {
        if (auto* found = qobject_cast<QStackedWidget*>(ancestor)) {
            stack = found;
            break;
        }
        chain.push_back(ancestor);
    }
    QWidget* const page = chain.back();
    const QWidget* const current =
        stack == nullptr ? nullptr : stack->currentWidget();
    if (current == nullptr)
        return;   //no stacked container: size() is all we have
    if (current == page)
        return;   //our page is current: it already holds the rect
    const QRect pageRect = current->geometry();
    //An empty rect means the stack has not laid anything out yet: the
    //placeholder is a better guess than zero, and the next feed re-syncs.
    if (!pageRect.isValid() || pageRect.isEmpty())
        return;
    if (page->geometry() == pageRect)
        return;
    //A hidden widget receives no resize event, so its layout never
    //runs on its own: push the rect down one container level at a
    //time, invalidating before each activate so revisits (window
    //resized while this page stayed hidden) defeat activate()'s
    //already-activated short-circuit. activate() computes from each
    //widget's CURRENT size, so the order is outermost first.
    page->setGeometry(pageRect);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        QLayout* layout = (*it)->layout();
        if (layout == nullptr)
            continue;   //the terminal itself (leaf) or a bare container
        layout->invalidate();
        layout->activate();
    }
}

void TerminalWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    //First show: the grid adopts the real geometry (hidden pages adopt
    //it lazily at feed time — see feedBytes).
    relayoutGrid();
}

void TerminalWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    //The real-geometry gate lives inside relayoutGrid.
    relayoutGrid();
}

void TerminalWidget::repaintDamaged() {
    int firstRow = -1, lastRow = -1;
    m_emulator.TakeDamageRange(&firstRow, &lastRow);
    //Whole-screen damage means the content scrolled: a selection stored
    //in display coordinates no longer points at what it was made on.
    if (firstRow == 0 && lastRow == rows() - 1)
        clearSelection();
    const bool cursorMoved = m_emulator.cursorMoved();
    m_emulator.ClearCursorMoved();
    if (m_scrollOffset != 0) {
        //Scrolled back: screen rows render displaced by the offset, so
        //row-accurate rectangles would land in the wrong place — repaint
        //everything, still tracking the cursor for the next repaint.
        if (cursorMoved) {
            m_lastCursorRow = m_emulator.cursorRow();
            m_lastCursorColumn = m_emulator.cursorColumn();
        }
        update();
        return;
    }
    QRect updateRect;
    if (firstRow >= 0)
        updateRect = QRect(0, kCellPadding + firstRow * m_cellHeight,
                           width(),
                           (lastRow - firstRow + 1) * m_cellHeight);
    if (cursorMoved) {
        //Repaint the cursor's old cell too, not just the new one.
        updateRect = updateRect.united(cursorRect()).united(
            cursorBlockRect(m_lastCursorColumn, m_lastCursorRow));
        m_lastCursorRow = m_emulator.cursorRow();
        m_lastCursorColumn = m_emulator.cursorColumn();
    }
    if (!updateRect.isNull())
        update(updateRect);
}

void TerminalWidget::blinkCursor() {
    m_cursorVisible = !m_cursorVisible;
    update(cursorRect());
}

bool TerminalWidget::hasSelection() const {
    return m_selectionAnchorRow >= 0
        && (m_selectionAnchorRow != m_selectionExtentRow
            || m_selectionAnchorColumn != m_selectionExtentColumn);
}

QString TerminalWidget::selectedText() const {
    if (!hasSelection()) return QString();
    const int firstRow =
        std::min(m_selectionAnchorRow, m_selectionExtentRow);
    const int lastRow =
        std::max(m_selectionAnchorRow, m_selectionExtentRow);
    const int firstColumn =
        std::min(m_selectionAnchorColumn, m_selectionExtentColumn);
    const int lastColumn =
        std::max(m_selectionAnchorColumn, m_selectionExtentColumn);
    QString text;
    for (int row = firstRow; row <= lastRow; ++row) {
        if (row > firstRow) text += QLatin1Char('\n');
        QString rowText;
        int column = firstColumn;
        while (column <= lastColumn && column < columns()) {
            const TermCell cell =
                m_emulator.CellAt(row - m_scrollOffset, column);
            rowText += CellText(cell);
            column += cell.width > 0 ? cell.width : 1;
        }
        while (rowText.endsWith(QLatin1Char(' ')))
            rowText.chop(1);
        text += rowText;
    }
    return text;
}

void TerminalWidget::copySelection() {
    const QString text = selectedText();
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

void TerminalWidget::clearSelection() {
    if (m_selectionAnchorRow < 0 && !m_selecting) return;
    m_selecting = false;
    m_selectionAnchorRow = -1;
    m_selectionAnchorColumn = -1;
    m_selectionExtentRow = -1;
    m_selectionExtentColumn = -1;
    update();
}

bool TerminalWidget::isSelected(int displayRow, int column) const {
    if (!hasSelection()) return false;
    const int firstRow =
        std::min(m_selectionAnchorRow, m_selectionExtentRow);
    const int lastRow =
        std::max(m_selectionAnchorRow, m_selectionExtentRow);
    const int firstColumn =
        std::min(m_selectionAnchorColumn, m_selectionExtentColumn);
    const int lastColumn =
        std::max(m_selectionAnchorColumn, m_selectionExtentColumn);
    return displayRow >= firstRow && displayRow <= lastRow
        && column >= firstColumn && column <= lastColumn;
}

void TerminalWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_selecting = true;
    m_selectionAnchorRow = rowAt(event->pos().y());
    m_selectionAnchorColumn = columnAt(event->pos().x());
    m_selectionExtentRow = m_selectionAnchorRow;
    m_selectionExtentColumn = m_selectionAnchorColumn;
    update();
    event->accept();
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!m_selecting) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    m_selectionExtentRow = rowAt(event->pos().y());
    m_selectionExtentColumn = columnAt(event->pos().x());
    update();
    event->accept();
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !m_selecting) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_selectionExtentRow = rowAt(event->pos().y());
    m_selectionExtentColumn = columnAt(event->pos().x());
    m_selecting = false;
    if (m_selectionAnchorRow == m_selectionExtentRow
        && m_selectionAnchorColumn == m_selectionExtentColumn)
        clearSelection();   //a click, not a selection
    update();
    event->accept();
}

void TerminalWidget::wheelEvent(QWheelEvent* event) {
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0) {
        event->ignore();
        return;
    }
    const int maxOffset = int(m_emulator.scrollback().size());
    const int newOffset =
        qBound(0, m_scrollOffset + notches * kWheelLinesPerNotch,
               maxOffset);
    if (newOffset != m_scrollOffset) {
        m_scrollOffset = newOffset;
        update();   //scroll view repaints everything
    }
    event->accept();
}

QPoint TerminalWidget::cellCenter(int column, int row) const {
    return QPoint(kCellPadding + column * m_cellWidth + m_cellWidth / 2,
                  kCellPadding + row * m_cellHeight
                      + m_cellHeight / 2);
}

//The single grid-geometry primitive for painting: the rect of a run
//whose first cell is (column, row), spanning `spanCells` cells. The
//origin stride is ALWAYS one cell — only the width widens for a span
//(wide glyph, line prompt). Two defects follow from violating that
//invariant, and the signature makes both unrepresentable: multiplying
//the stride by the span shifts every run not at column 0 to the right
//(the wide-glyph offset bug), and ignoring the span clips a wide glyph
//to one cell (the wide-glyph clip bug).
QRect TerminalWidget::cellRunRect(int column, int row,
                                  int spanCells) const {
    return QRect(kCellPadding + column * m_cellWidth,
                 kCellPadding + row * m_cellHeight,
                 m_cellWidth * spanCells, m_cellHeight);
}

//The block the cursor inverts: the cell under (column, row) widened to
//that cell's own span, so a wide glyph's block covers both grid cells.
//The paint pass, the blink repaint and the damage rect of the previous
//cursor position must all use this rect — a one-cell rect leaves the
//right half of a wide block on screen after the blink-off pass, and the
//same residue behind when the cursor moves off a wide cell.
QRect TerminalWidget::cursorBlockRect(int column, int row) const {
    if (row < 0 || row >= rows() || column < 0 || column >= columns())
        return QRect();
    const TermCell cell = m_emulator.CellAt(row, column);
    return cellRunRect(column, row, cell.width > 0 ? cell.width : 1);
}

QRect TerminalWidget::cursorRect() const {
    if (m_scrollOffset != 0) return QRect();   //scrolled back: no cursor
    return cursorBlockRect(m_emulator.cursorColumn(), m_emulator.cursorRow());
}

int TerminalWidget::columnAt(int x) const {
    return qBound(0, (x - kCellPadding) / m_cellWidth, columns() - 1);
}

int TerminalWidget::rowAt(int y) const {
    return qBound(0, (y - kCellPadding) / m_cellHeight, rows() - 1);
}

} // namespace terminal
} // namespace nlang
