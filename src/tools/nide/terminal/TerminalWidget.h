/*---
TerminalWidget.h — the terminal viewport: a monospace cell grid drawn
with QPainter over the TerminalEmulator state. This task's scope:
rendering, cell-granular selection, wheel scrollback, cursor blink and
grid relayout. The input modes (character passthrough / line editing)
and IME handling join in a later task by editing this header.
---*/
#pragma once
#include <QWidget>

#include <QFont>
#include <QPoint>
#include <QRect>

#include "TerminalEmulator.h"

class QPainter;
class QTimer;

namespace nlang {
namespace terminal {

inline constexpr int kCellPadding = 2;   //grid margin, pixels
inline constexpr int kMinColumns = 20;   //relayout floors
inline constexpr int kMinRows = 5;

//One cell's text as a QString (surrogate pairs for non-BMP scalars).
//Shared by the paint pass and selection-text extraction.
inline QString CellText(const TermCell& cell) {
    QString text;
    for (int i = 0; i < cell.charCount; ++i) {
        const uint32_t codePoint = cell.chars[i];
        if (codePoint < 0x10000) {
            text.append(static_cast<unsigned short>(codePoint));
        } else {
            const uint32_t value = codePoint - 0x10000;
            text.append(static_cast<unsigned short>(
                0xD800 | (value >> 10)));
            text.append(static_cast<unsigned short>(
                0xDC00 | (value & 0x3FF)));
        }
    }
    return text;
}

class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget* parent = nullptr);

    //Program output in (VT bytes): feed the emulator, repaint damage.
    void feedBytes(const QByteArray& vtBytes);
    void feedUtf8(const QString& text);
    //Fresh session: clear screen + scrollback + selection.
    void resetTerminal();

    int columns() const { return m_emulator.columns(); }
    int rows() const { return m_emulator.rows(); }
    std::string screenText() const { return m_emulator.ScreenText(); }

    bool hasSelection() const;
    QString selectedText() const;

    //Geometry readouts (tests, IME cursor rectangle).
    int cellWidth() const { return m_cellWidth; }
    int cellHeight() const { return m_cellHeight; }
    QPoint cellCenter(int column, int row) const;
    QRect cursorRect() const;

    void setByteSink(TerminalEmulator::ByteSink sink) {
        m_emulator.SetByteSink(std::move(sink));
    }

    QSize sizeHint() const override;

public slots:
    void copySelection();

signals:
    //Grid geometry changed after a relayout (columns, rows).
    void sizeChanged(int columns, int rows);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private slots:
    void blinkCursor();

private:
    //Defined in TerminalWidgetPaint.cpp.
    void drawCell(QPainter& painter, int displayRow, int column,
                  const TermCell& cell);
    void drawCursorBlock(QPainter& painter);
    //Defined in TerminalWidget.cpp.
    void relayoutGrid();
    void repaintDamaged();
    void clearSelection();
    bool isSelected(int displayRow, int column) const;
    int columnAt(int x) const;
    int rowAt(int y) const;

    TerminalEmulator m_emulator;
    int m_cellWidth = 1;
    int m_cellHeight = 1;
    QFont m_boldFont;
    QTimer* m_pBlinkTimer = nullptr;
    bool m_cursorVisible = true;
    int m_lastCursorRow = 0;
    int m_lastCursorColumn = 0;
    int m_scrollOffset = 0;   //scrollback lines shown above the screen
    //Selection, display coordinates; dropped when output scrolls the
    //screen (full-damage feed) — MVP limitation, static-screen use only.
    bool m_selecting = false;
    int m_selectionAnchorRow = -1;
    int m_selectionAnchorColumn = -1;
    int m_selectionExtentRow = -1;
    int m_selectionExtentColumn = -1;
};

} // namespace terminal
} // namespace nlang
