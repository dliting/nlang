/*---
TerminalWidgetPaint.cpp — the terminal's QPainter pass: cell grid with
colours and attributes, selection highlight, cursor block. Split from
TerminalWidget.cpp to keep both under the source size guard.
---*/
#include "TerminalWidget.h"

#include <QPainter>

#include "TerminalPalette.h"

namespace nlang {
namespace terminal {
namespace {

QColor ToColor(unsigned int rgb) {
    return QColor(static_cast<int>((rgb >> 16) & 0xFF),
                  static_cast<int>((rgb >> 8) & 0xFF),
                  static_cast<int>(rgb & 0xFF));
}

constexpr unsigned int kSelectionBackground = 0x264F78;

QRect CellRect(int column, int row, int cellWidth, int cellHeight) {
    return QRect(kCellPadding + column * cellWidth,
                 kCellPadding + row * cellHeight,
                 cellWidth, cellHeight);
}

} // namespace

void TerminalWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), ToColor(kDefaultBackground));
    for (int displayRow = 0; displayRow < rows(); ++displayRow) {
        //Scroll-offset addressing: displayRow - m_scrollOffset is a
        //screen row while following the bottom, and the same expression
        //goes negative (scrollback indexing) while scrolled back.
        const int emulatorRow = displayRow - m_scrollOffset;
        int column = 0;
        while (column < columns()) {
            const TermCell cell = m_emulator.CellAt(emulatorRow, column);
            drawCell(painter, displayRow, column, cell);
            column += cell.width > 0 ? cell.width : 1;
        }
    }
    drawCursorBlock(painter);
}

void TerminalWidget::drawCell(QPainter& painter, int displayRow,
                              int column, const TermCell& cell) {
    const QRect cellRect =
        CellRect(column, displayRow, m_cellWidth, m_cellHeight);
    unsigned int foreground = cell.foreground;
    unsigned int background = cell.background;
    if (cell.reverse) std::swap(foreground, background);
    if (isSelected(displayRow, column))
        background = kSelectionBackground;
    if (background != kDefaultBackground)
        painter.fillRect(cellRect, ToColor(background));
    if (cell.charCount == 0) return;
    painter.setPen(ToColor(foreground));
    painter.setFont(cell.bold ? m_boldFont : font());
    painter.drawText(cellRect, Qt::AlignLeft | Qt::AlignVCenter,
                     CellText(cell));
    if (cell.underline != VTERM_UNDERLINE_OFF) {
        const int y = cellRect.bottom() - 2;
        painter.drawLine(cellRect.left(), y, cellRect.right(), y);
    }
}

void TerminalWidget::drawCursorBlock(QPainter& painter) {
    if (m_scrollOffset != 0 || !m_cursorVisible) return;
    const int row = m_emulator.cursorRow();
    const int column = m_emulator.cursorColumn();
    if (row < 0 || row >= rows() || column < 0 || column >= columns())
        return;
    const TermCell cell = m_emulator.CellAt(row, column);
    const QRect cellRect =
        CellRect(column, row, m_cellWidth, m_cellHeight);
    //A wide char's cursor spans its two cells.
    const int span = m_cellWidth * (cell.width > 0 ? cell.width : 1);
    painter.fillRect(cellRect.adjusted(0, 0, span - m_cellWidth, 0),
                     ToColor(kDefaultForeground));
    if (cell.charCount > 0) {
        painter.setPen(ToColor(kDefaultBackground));
        painter.setFont(cell.bold ? m_boldFont : font());
        painter.drawText(cellRect, Qt::AlignLeft | Qt::AlignVCenter,
                         CellText(cell));
    }
}

} // namespace terminal
} // namespace nlang
