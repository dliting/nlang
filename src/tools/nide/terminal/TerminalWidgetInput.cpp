/*---
TerminalWidgetInput.cpp — the terminal's keyboard/IME/paste input:
Ctrl+C/V shortcuts, per-mode key dispatch (Character passthrough with
Qt-to-VTerm key mapping, Line editing over LineEditor), IME preedit
and commit, paste flattening. Split from TerminalWidget.cpp to keep
both under the source size guard (Paint split precedent).
---*/
#include "TerminalWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QTimer>

#include <vterm.h>

namespace nlang {
namespace terminal {
namespace {

//Qt key code -> VTerm key code; VTERM_KEY_NONE for "no special key"
//(printable text travels as text, not as a key).
VTermKey MapQtKey(int key) {
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:     return VTERM_KEY_ENTER;
    case Qt::Key_Tab:       return VTERM_KEY_TAB;
    case Qt::Key_Backspace: return VTERM_KEY_BACKSPACE;
    case Qt::Key_Escape:    return VTERM_KEY_ESCAPE;
    case Qt::Key_Up:        return VTERM_KEY_UP;
    case Qt::Key_Down:      return VTERM_KEY_DOWN;
    case Qt::Key_Left:      return VTERM_KEY_LEFT;
    case Qt::Key_Right:     return VTERM_KEY_RIGHT;
    case Qt::Key_Home:      return VTERM_KEY_HOME;
    case Qt::Key_End:       return VTERM_KEY_END;
    case Qt::Key_PageUp:    return VTERM_KEY_PAGEUP;
    case Qt::Key_PageDown:  return VTERM_KEY_PAGEDOWN;
    case Qt::Key_Insert:    return VTERM_KEY_INS;
    case Qt::Key_Delete:    return VTERM_KEY_DEL;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
            return static_cast<VTermKey>(
                VTERM_KEY_FUNCTION(key - Qt::Key_F1 + 1));
        return VTERM_KEY_NONE;
    }
}

} // namespace

void TerminalWidget::setMode(Mode mode) {
    if (m_mode == mode) return;
    m_mode = mode;
    m_preedit.clear();
    if (mode == Mode::Line) m_editor.clear();
    restartCursorBlink();
    update();
}

QString TerminalWidget::pendingLine() const {
    const std::string& line = m_editor.line();
    const std::size_t cursor = m_editor.cursorByte();
    //Line prefix + IME preedit at the caret + suffix: what Line mode
    //is locally composing, before any echo.
    QString out = QString::fromUtf8(line.data(), int(cursor));
    out += m_preedit;
    out += QString::fromUtf8(line.data() + cursor,
                             int(line.size() - cursor));
    return out;
}

bool TerminalWidget::handleClipboardKey(QKeyEvent* event) {
    //The Windows Terminal copy/paste family, ahead of the mode
    //dispatch so the bindings hold in every mode. Ctrl+Shift+C must be
    //tested BEFORE plain Ctrl+C (that branch would otherwise eat the
    //chord as the dual-meaning copy/interrupt key); the Insert pairs
    //are intercepted here so they never reach MapQtKey, which would
    //encode them as the INSERT key for the child.
    const int key = event->key();
    const bool ctrl =
        (event->modifiers() & Qt::ControlModifier) != 0;
    const bool shift =
        (event->modifiers() & Qt::ShiftModifier) != 0;
    if (ctrl && shift && key == Qt::Key_C) {
        copySelection();   //always copy; without a selection a no-op
        return true;
    }
    if (ctrl && key == Qt::Key_C) {
        if (hasSelection()) {
            copySelection();   //copy wins over interrupt
        } else if (m_mode == Mode::Character) {
            //No selection: the terminal interrupt, sent as a raw C0
            //byte (neovim terminal.c precedent).
            m_emulator.SendChar(0x03, VTERM_MOD_NONE);
            restartCursorBlink();
        }
        return true;
    }
    if (ctrl && key == Qt::Key_V) {   //covers Ctrl+Shift+V as well
        pasteClipboard();
        return true;
    }
    if (ctrl && !shift && key == Qt::Key_Insert) {
        copySelection();
        return true;
    }
    if (shift && !ctrl && key == Qt::Key_Insert) {
        pasteClipboard();
        return true;
    }
    return false;
}

void TerminalWidget::keyPressEvent(QKeyEvent* event) {
    if (handleClipboardKey(event)) {
        event->accept();
        return;
    }
    switch (m_mode) {
    case Mode::Character:
        characterKey(event);
        return;
    case Mode::Line:
        lineModeKey(event);
        return;
    case Mode::Idle:
        break;   //no input consumed, no buffering
    }
    QWidget::keyPressEvent(event);
}

void TerminalWidget::characterKey(QKeyEvent* event) {
    const VTermKey key = MapQtKey(event->key());
    if (key != VTERM_KEY_NONE) {
        m_emulator.SendKey(key, VTERM_MOD_NONE);
    } else if (!event->text().isEmpty()) {
        m_emulator.SendText(event->text().toUtf8().toStdString());
    } else {
        QWidget::keyPressEvent(event);   //nothing encodable
        return;
    }
    restartCursorBlink();
    event->accept();
}

void TerminalWidget::lineModeKey(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        commitLine();
        break;
    case Qt::Key_Backspace:
        m_editor.backspace();
        break;
    case Qt::Key_Left:
        m_editor.moveCursor(-1);
        break;
    case Qt::Key_Right:
        m_editor.moveCursor(1);
        break;
    case Qt::Key_Up:
        m_editor.historyPrev();
        break;
    case Qt::Key_Down:
        m_editor.historyNext();
        break;
    case Qt::Key_Tab:
        break;   //no completion in line mode; swallow
    default:
        if (event->text().isEmpty()) {
            QWidget::keyPressEvent(event);
            return;
        }
        m_editor.insertText(event->text().toUtf8().toStdString());
        break;
    }
    restartCursorBlink();
    update();   //pending-line repaint
    event->accept();
}

void TerminalWidget::commitLine() {
    //By value: m_editor.clear() below would empty the referenced
    //buffer before the echo reads it.
    const std::string line = m_editor.line();
    if (!m_editor.empty()) {
        m_editor.commit();
        emit lineCommitted(
            QString::fromUtf8(line.data(), int(line.size())));
    }
    m_editor.clear();
    //Synthetic echo: the committed line enters the screen (and thus
    //the scrollback history) only at Enter — spec §4. Non-empty lines
    //carry the prompt, so history reads like live input; an empty
    //line stays empty (a lone newline echo).
    m_emulator.Feed(line.empty()
                        ? std::string("\r\n")
                        : std::string(kLinePrompt) + line + "\r\n");
    repaintDamaged();
}

void TerminalWidget::pasteClipboard() {
    pasteText(QApplication::clipboard()->text());
}

void TerminalWidget::pasteText(const QString& text) {
    if (text.isEmpty()) return;
    if (m_mode == Mode::Character) {
        m_emulator.SendPaste(text.toUtf8().toStdString());
        restartCursorBlink();
    } else if (m_mode == Mode::Line) {
        //The editor holds one line: flatten every line break (CRLF
        //first, then lone LF/CR) to single spaces.
        QString flat = text;
        flat.remove(QLatin1Char('\r'));
        flat.replace(QLatin1Char('\n'), QLatin1Char(' '));
        m_editor.insertText(flat.toUtf8().toStdString());
        restartCursorBlink();
        update();
    }
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent* event) {
    //Preedit replaces wholesale; the commit string is consumed per
    //mode exactly like typed text.
    m_preedit = event->preeditString();
    const QString commit = event->commitString();
    if (!commit.isEmpty()) {
        const std::string bytes = commit.toUtf8().toStdString();
        if (m_mode == Mode::Character) {
            m_emulator.SendText(bytes);
            restartCursorBlink();
        } else if (m_mode == Mode::Line) {
            m_editor.insertText(bytes);
            restartCursorBlink();
        }
    }
    update();
    event->accept();
}

QVariant TerminalWidget::inputMethodQuery(
    Qt::InputMethodQuery query) const {
    switch (query) {
    case Qt::ImEnabled:
        return QVariant(true);
    case Qt::ImCursorRectangle:
        //The composition window follows the grid cursor.
        return QVariant(cursorRect());
    default:
        return QWidget::inputMethodQuery(query);
    }
}

void TerminalWidget::restartCursorBlink() {
    m_cursorVisible = true;
    m_pBlinkTimer->start();
    update(cursorRect());
}

} // namespace terminal
} // namespace nlang
