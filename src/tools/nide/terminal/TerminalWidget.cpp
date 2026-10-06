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
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

namespace nlang {
namespace terminal {
namespace {

constexpr int kCursorBlinkIntervalMs = 530;
constexpr int kWheelLinesPerNotch = 3;
constexpr int kDefaultColumns = 80;   //initial sizeHint grid
constexpr int kDefaultRows = 24;

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
    relayoutGrid();
}

QSize TerminalWidget::sizeHint() const {
    return QSize(kCellPadding * 2 + kDefaultColumns * m_cellWidth,
                 kCellPadding * 2 + kDefaultRows * m_cellHeight);
}

void TerminalWidget::feedBytes(const QByteArray& vtBytes) {
    m_emulator.Feed(std::string(vtBytes.constData(),
                                static_cast<std::size_t>(vtBytes.size())));
    repaintDamaged();
}

void TerminalWidget::feedUtf8(const QString& text) {
    feedBytes(text.toUtf8());
}

void TerminalWidget::resetTerminal() {
    m_emulator.Reset();
    m_scrollOffset = 0;
    clearSelection();
    repaintDamaged();   //Reset() reports full damage + cursor move
}

void TerminalWidget::relayoutGrid() {
    const QFontMetrics metrics(font());
    m_cellWidth = std::max(1, metrics.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = std::max(1, metrics.lineSpacing());
    m_boldFont = font();
    m_boldFont.setBold(true);
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

void TerminalWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
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
        updateRect = updateRect.united(cursorRect()).united(QRect(
            kCellPadding + m_lastCursorColumn * m_cellWidth,
            kCellPadding + m_lastCursorRow * m_cellHeight,
            m_cellWidth, m_cellHeight));
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

QRect TerminalWidget::cursorRect() const {
    if (m_scrollOffset != 0) return QRect();   //scrolled back: no cursor
    const int row = m_emulator.cursorRow();
    const int column = m_emulator.cursorColumn();
    if (row < 0 || row >= rows() || column < 0 || column >= columns())
        return QRect();
    return QRect(kCellPadding + column * m_cellWidth,
                 kCellPadding + row * m_cellHeight,
                 m_cellWidth, m_cellHeight);
}

int TerminalWidget::columnAt(int x) const {
    return qBound(0, (x - kCellPadding) / m_cellWidth, columns() - 1);
}

int TerminalWidget::rowAt(int y) const {
    return qBound(0, (y - kCellPadding) / m_cellHeight, rows() - 1);
}

} // namespace terminal
} // namespace nlang
