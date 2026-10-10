/*---
    TerminalWidgetMouse.cpp — mouse interaction: text selection,
    Ctrl+click source links (v0.8.5), Ctrl+hover underline state.
    Split from TerminalWidget.cpp to respect the source-size guard.
---*/
#include "TerminalWidget.h"

#include <QMouseEvent>

#include <vector>

namespace nlang {
namespace terminal {

void TerminalWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton
        && (event->modifiers() & Qt::ControlModifier)) {
        //v0.8.5: Ctrl+click follows a source link (the editor's
        //go-to-definition mirror); plain clicks keep selection.
        const SourceLink link = linkAtCell(
            rowAt(event->pos().y()), columnAt(event->pos().x()));
        if (link.isValid() && link.line > 0)
            emit sourceLinkActivated(link.filePath, link.line);
        event->accept();
        return;
    }
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
    if (!m_selecting && (event->modifiers() & Qt::ControlModifier)) {
        //Ctrl+hover: pointing hand over a link, underline pass armed.
        int firstColumn = -1, lastColumn = -1;
        const int row = rowAt(event->pos().y());
        const SourceLink link = linkAtCell(row,
            columnAt(event->pos().x()), &firstColumn, &lastColumn);
        setCursor(link.isValid() ? Qt::PointingHandCursor
                                 : Qt::ArrowCursor);
        const int linkRow = link.isValid() ? row : -1;
        if (linkRow != m_hoverRow || firstColumn != m_hoverFirstColumn
            || lastColumn != m_hoverLastColumn)
            emitHoverUnderline(linkRow, firstColumn, lastColumn);
        event->accept();
        return;
    }
    if (!m_selecting
        && (m_hoverRow >= 0 || cursor().shape() != Qt::ArrowCursor)) {
        setCursor(Qt::ArrowCursor);
        emitHoverUnderline(-1, -1, -1);
    }
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

void TerminalWidget::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    if (m_hoverRow >= 0 || cursor().shape() != Qt::ArrowCursor) {
        setCursor(Qt::ArrowCursor);
        emitHoverUnderline(-1, -1, -1);
    }
}

void TerminalWidget::emitHoverUnderline(int displayRow, int firstColumn,
                                        int lastColumn) {
    m_hoverRow = displayRow;
    m_hoverFirstColumn = firstColumn;
    m_hoverLastColumn = lastColumn;
    update();
}

//v0.8.5: hit-test core. Builds the row text WITH a per-character grid
//column map (a wide glyph owns two cells) so a matched token's QString
//span maps exactly onto grid columns.
SourceLink TerminalWidget::linkAtCell(int displayRow, int column,
                                      int* firstColumn,
                                      int* lastColumn) const {
    if (firstColumn) *firstColumn = -1;
    if (lastColumn) *lastColumn = -1;
    const int emulatorRow = displayRow - m_scrollOffset;
    QString rowText;
    std::vector<int> charColumns;   //grid column each QChar starts at
    for (int col = 0; col < columns(); ) {
        const TermCell cell = m_emulator.CellAt(emulatorRow, col);
        const int span = cell.width > 0 ? cell.width : 1;
        const QString text = CellText(cell);
        for (int i = 0; i < text.size(); ++i)
            charColumns.push_back(col);
        rowText += text;
        col += span;
    }
    const QVector<SourceLink> links = FindSourceLinks(rowText);
    for (const SourceLink& link : links) {
        const int first = charColumns[static_cast<size_t>(link.firstChar)];
        //Mid-link wide glyphs: their second cell sits before the next
        //char's column, already inside [first, lastColumn]. Only the
        //LAST char may own a trailing second cell — extend by its
        //actual cell width.
        const int lastStart =
            charColumns[static_cast<size_t>(link.lastChar)];
        const TermCell lastCell =
            m_emulator.CellAt(emulatorRow, lastStart);
        const int last = lastStart
            + (lastCell.width > 1 ? lastCell.width - 1 : 0);
        if (column < first || column > last)
            continue;
        if (firstColumn) *firstColumn = first;
        if (lastColumn) *lastColumn = last;
        return link;
    }
    return SourceLink{};
}

} // namespace terminal
} // namespace nlang
