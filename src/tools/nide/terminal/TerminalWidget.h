/*---
TerminalWidget.h — the terminal viewport: a monospace cell grid drawn
with QPainter over the TerminalEmulator state. Rendering, selection,
wheel scrollback, cursor blink, grid relayout, plus the dual input
modes: Character (keys encoded straight through to the child) and Line
(local line editing with history; Enter commits and echoes). IME
preedit/commit is handled per mode.
---*/
#pragma once
#include <QWidget>

#include <QFont>
#include <QPoint>
#include <QRect>

#include "LineEditor.h"
#include "TerminalEmulator.h"

class QInputMethodEvent;
class QKeyEvent;
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
    //Idle: no input consumed. Character: passthrough to the child.
    //Line: local editing; Enter commits (lineCommitted) and echoes.
    enum class Mode { Idle, Character, Line };

    explicit TerminalWidget(QWidget* parent = nullptr);

    //Program output in (VT bytes): feed the emulator, repaint damage.
    void feedBytes(const QByteArray& vtBytes);
    void feedUtf8(const QString& text);
    //Fresh session: clear screen + scrollback + selection.
    void resetTerminal();

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    //The line-mode pending line with the IME preedit at the caret —
    //the local editing state, before any echo.
    QString pendingLine() const;

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

    //Mode-aware paste dispatch (pasteClipboard without the clipboard):
    //Character sends upstream with CRLF collapsed, Line flattens
    //newlines to spaces and edits them in.
    void pasteText(const QString& text);

public slots:
    void copySelection();
    void pasteClipboard();

signals:
    //Grid geometry changed after a relayout (columns, rows).
    void sizeChanged(int columns, int rows);
    //Line mode: Enter on a non-empty pending line.
    void lineCommitted(const QString& line);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
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
    void drawPendingLine(QPainter& painter);
    bool drawPendingRun(QPainter& painter, const QString& run,
                        int& row, int& column, bool preedit);
    //Defined in TerminalWidgetInput.cpp.
    void characterKey(QKeyEvent* event);
    void lineModeKey(QKeyEvent* event);
    void commitLine();
    void restartCursorBlink();
    //Defined in TerminalWidget.cpp.
    void relayoutGrid();
    void updateCellMetrics();
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
    //False until the first showEvent: pre-show widget geometry is the
    //layout's placeholder (not a real terminal size), so the emulator
    //keeps its own default grid until the widget is on screen.
    bool m_hasShown = false;
    Mode m_mode = Mode::Idle;
    LineEditor m_editor;      //Line-mode editing state
    QString m_preedit;        //IME composition text at the caret
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
