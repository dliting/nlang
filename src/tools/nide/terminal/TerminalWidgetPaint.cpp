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

void TerminalWidget::drawPendingLine(QPainter& painter) {
    if (m_mode != Mode::Line || m_scrollOffset != 0) return;
    const std::string& line = m_editor.line();
    const std::size_t cursorByte = m_editor.cursorByte();
    const QString left = QString::fromUtf8(line.data(),
                                           int(cursorByte));
    const QString right =
        QString::fromUtf8(line.data() + cursorByte,
                          int(line.size() - cursorByte));
    //Drawing starts at the live cursor cell (where output would go).
    int row = m_emulator.cursorRow();
    int column = m_emulator.cursorColumn();
    if (!drawPendingRun(painter, left, row, column, false)) return;
    //Caret: a 2px vertical bar at the edit position.
    if (row < rows() && column < columns()) {
        QRect caret = CellRect(column, row, m_cellWidth, m_cellHeight);
        caret.setWidth(2);
        painter.fillRect(caret, ToColor(kDefaultForeground));
    }
    if (!drawPendingRun(painter, m_preedit, row, column, true)) return;
    drawPendingRun(painter, right, row, column, false);
}

bool TerminalWidget::drawPendingRun(QPainter& painter, const QString& run,
                                    int& row, int& column, bool preedit) {
    //One scalar per step; wide scalars advance two cells (the same
    //skip strategy as the grid pass). False once past the bottom.
    const QFontMetrics metrics = painter.fontMetrics();
    for (int i = 0; i < run.size();) {
        QString scalar;
        const QChar lead = run.at(i);
        if (lead.isHighSurrogate() && i + 1 < run.size()) {
            scalar = run.mid(i, 2);
            i += 2;
        } else {
            scalar = QString(lead);
            i += 1;
        }
        if (row >= rows()) return false;      //bottom truncation
        if (column >= columns()) {            //wrap at line full
            column = 0;
            ++row;
            if (row >= rows()) return false;
        }
        const QRect cellRect =
            CellRect(column, row, m_cellWidth, m_cellHeight);
        painter.setPen(ToColor(kDefaultForeground));
        painter.drawText(cellRect, Qt::AlignLeft | Qt::AlignVCenter,
                         scalar);
        if (preedit) {   //composition text is underlined
            const int y = cellRect.bottom() - 2;
            painter.drawLine(cellRect.left(), y, cellRect.right(), y);
        }
        column += metrics.horizontalAdvance(scalar) > m_cellWidth
                      ? 2 : 1;
    }
    return true;
}

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
    drawPendingLine(painter);
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
    //Line mode shows its own caret inside the pending line; the block
    //cursor belongs to Character mode only.
    if (m_mode != Mode::Character || m_scrollOffset != 0
        || !m_cursorVisible)
        return;
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
