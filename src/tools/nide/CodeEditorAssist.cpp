/*--- CodeEditorAssist.cpp - library-backed code assistance of the NLang IDE
    code editor: symbol-index wiring, hover help and go-to-definition (F12,
    F6 and Ctrl+click, with the pointing-hand hover preview), plus the key
    policy of the completion popup (whose list lives in
    CodeEditorCompletion.cpp: typing narrows it, Escape or any other key
    dismisses it). ---*/
#include "CodeEditor.h"

#include "nlang/langservice/SymbolIndex.h"

#include <QCoreApplication>
#include <QHelpEvent>
#include <QMenu>
#include <QKeyEvent>
#include <QTextBlock>
#include <QToolTip>

namespace nlang {

void CodeEditor::setSymbolIndex(const langservice::SymbolIndex* index) {
    m_symbolIndex = index;
    //A freshly indexed package can make the cursor's token jumpable;
    //re-announce the availability.
    emit jumpTargetAvailable(jumpTargetAt(textCursor()).has_value());
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

QString CodeEditor::qualifiedNameAt(const QString& lineText, int column,
                                    int* start, int* length) {
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

    if (start != nullptr)
        *start = chainStart;
    if (length != nullptr)
        *length = end - chainStart;
    return lineText.mid(chainStart, p - chainStart)
         + lineText.mid(nameStart, end - nameStart);
}

QString CodeEditor::importPackageAt(const QString& lineText, int column,
                                    int* start, int* length) {
    auto isIdent = [](QChar ch) {
        return ch.isLetterOrNumber() || ch == '_';
    };
    auto isIdentStart = [](QChar ch) {
        return ch.isLetter() || ch == '_';
    };
    const int len = lineText.length();
    int pos = 0;
    while (pos < len && lineText.at(pos).isSpace())
        ++pos;
    if (lineText.mid(pos, 6) != QLatin1String("import"))
        return QString();
    pos += 6;
    //The keyword must be its own token ("imports" is not import).
    if (pos < len && isIdent(lineText.at(pos)))
        return QString();
    while (pos < len && lineText.at(pos).isSpace())
        ++pos;
    if (pos >= len || !isIdentStart(lineText.at(pos)))
        return QString();
    //ident ('.' ident)* — a dot joins the chain only when another
    //identifier follows, so the wildcard of "import utils.*;" stays
    //outside it and the returned package is "utils", not "utils.".
    const int chainStart = pos;
    while (pos < len) {
        if (!isIdentStart(lineText.at(pos)))
            break;
        while (pos < len && isIdent(lineText.at(pos)))
            ++pos;
        if (pos + 1 < len && lineText.at(pos) == QLatin1Char('.')
            && isIdentStart(lineText.at(pos + 1)))
            ++pos;
        else
            break;
    }
    if (column < chainStart || column > pos)
        return QString();
    if (start != nullptr)
        *start = chainStart;
    if (length != nullptr)
        *length = pos - chainStart;
    return lineText.mid(chainStart, pos - chainStart);
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

const langservice::SymbolInfo* CodeEditor::resolveAt(
    const QTextCursor& cursor) const {
    if (m_symbolIndex == nullptr)
        return nullptr;
    const QString qualified = qualifiedNameAt(
        cursor.block().text(), cursor.positionInBlock());
    //The package is the whole dotted prefix, the name the last segment
    //(multi-segment packages: "vendor.graphics.hue").
    const int lastDot = qualified.lastIndexOf(QLatin1Char('.'));
    if (lastDot <= 0 || lastDot + 1 >= qualified.length())
        return nullptr;
    return m_symbolIndex->Resolve(qualified.left(lastDot).toStdString(),
                                  qualified.mid(lastDot + 1).toStdString());
}

std::optional<CodeEditor::JumpTarget> CodeEditor::jumpTargetAt(
    const QTextCursor& cursor) const {
    if (m_symbolIndex == nullptr)
        return std::nullopt;
    const QTextBlock block = cursor.block();
    if (const langservice::SymbolInfo* symbol = resolveAt(cursor)) {
        JumpTarget target;
        target.filePath = QString::fromStdString(symbol->filePath);
        target.line = symbol->line;
        int start = 0;
        int length = 0;
        qualifiedNameAt(block.text(), cursor.positionInBlock(),
                        &start, &length);
        target.tokenPos = block.position() + start;
        target.tokenLength = length;
        return target;
    }
    //Import statement: the package chain itself is the target — jump to
    //the file that declares it.
    int start = 0;
    int length = 0;
    const QString pkg = importPackageAt(block.text(),
                                        cursor.positionInBlock(),
                                        &start, &length);
    if (pkg.isEmpty())
        return std::nullopt;
    const std::string file =
        m_symbolIndex->PackageFilePath(pkg.toStdString());
    if (file.empty())
        return std::nullopt;
    JumpTarget target;
    target.filePath = QString::fromStdString(file);
    target.line = 1;
    target.tokenPos = block.position() + start;
    target.tokenLength = length;
    return target;
}

bool CodeEditor::hasJumpTargetAtCursor() const {
    return jumpTargetAt(textCursor()).has_value();
}

bool CodeEditor::goToDefinitionAtCursor() {
    return goToDefinitionAt(textCursor());
}

bool CodeEditor::goToDefinitionAt(const QTextCursor& cursor) {
    const std::optional<JumpTarget> target = jumpTargetAt(cursor);
    if (!target)
        return false;
    emit goToDefinitionRequested(target->filePath, target->line);
    return true;
}

bool CodeEditor::handleToolTip(QHelpEvent* helpEvent) {
    if (!m_symbolIndex) {
        QToolTip::hideText();
        return false;
    }
    const langservice::SymbolInfo* symbol =
        resolveAt(cursorForPosition(helpEvent->pos()));
    if (symbol) {
        QToolTip::showText(helpEvent->globalPos(),
                           formatSymbol(*symbol), this);
        return true;
    }
    QToolTip::hideText();
    return false;
}

void CodeEditor::mousePressEvent(QMouseEvent* event) {
    //Ctrl+click on a resolvable name goes to its definition (the
    //standard code-editor shortcut); the click is consumed so the
    //cursor stays where it was.
    if ((event->modifiers() & Qt::ControlModifier)
        && event->button() == Qt::LeftButton
        && goToDefinitionAt(cursorForPosition(event->pos()))) {
        return;
    }
    //A click in the editor repositions the cursor; the completion
    //popup would stay anchored at a stale position — close it. Focus
    //stays on the editor here, so focusOutEvent alone misses this path.
    closeCompletion();
    QPlainTextEdit::mousePressEvent(event);
}

void CodeEditor::mouseMoveEvent(QMouseEvent* event) {
    //The pointer position is remembered so that a Ctrl press or release
    //can re-evaluate the affordance without waiting for the next move.
    m_lastHoverPos = event->pos();
    updateLinkAffordance(event->pos(),
                         event->modifiers() & Qt::ControlModifier);
    QPlainTextEdit::mouseMoveEvent(event);
}

void CodeEditor::updateLinkAffordance(const QPoint& viewportPos,
                                      bool ctrlHeld) {
    //While Ctrl is held, a pointing hand marks the names a Ctrl+click
    //would jump to, and the token renders as a blue-underlined hyperlink
    //(the affordances that make the shortcut visible). A position
    //outside the viewport (the pointer rests elsewhere while Ctrl is
    //pressed) shows no affordance, exactly like a leave.
    const bool hovering = ctrlHeld
        && viewport()->rect().contains(viewportPos);
    const std::optional<JumpTarget> target = hovering
        ? jumpTargetAt(cursorForPosition(viewportPos)) : std::nullopt;
    viewport()->setCursor(target ? Qt::PointingHandCursor : Qt::IBeamCursor);
    setLinkHighlight(target ? target->tokenPos : -1,
                     target ? target->tokenLength : 0);
}

void CodeEditor::focusOutEvent(QFocusEvent* event) {
    //The completion popup belongs to this editor's focus: when focus
    //moves elsewhere (another panel, a dialog, another window) the
    //popup must go with it — like a menu, not a persistent overlay.
    closeCompletion();
    QPlainTextEdit::focusOutEvent(event);
}

void CodeEditor::leaveEvent(QEvent* event) {
    //The pointer left the editor: the Ctrl+hover link decoration must
    //not linger (the next in-editor move would clear it, but until then
    //a stale blue token would keep claiming to be a link).
    m_lastHoverPos = QPoint(-1, -1);
    setLinkHighlight(-1, 0);
    QPlainTextEdit::leaveEvent(event);
}

void CodeEditor::setLinkHighlight(int tokenPos, int tokenLength) {
    if (tokenPos == m_linkPos && tokenLength == m_linkLength)
        return;  //sweeping within one token: no rebuild churn
    m_linkPos = tokenPos;
    m_linkLength = tokenLength;
    highlightCurrentLine();  //rebuild the extra-selection list
}

void CodeEditor::contextMenuEvent(QContextMenuEvent* event) {
    QMenu* menu = createStandardContextMenu(event->pos());
    //Go to Definition heads the menu when the cursor sits on a jump
    //target (the same probe the Ctrl+click affordances use).
    if (hasJumpTargetAtCursor()) {
        QAction* jump = new QAction(tr("Go to Definition"), menu);
        connect(jump, &QAction::triggered, this,
                &CodeEditor::goToDefinitionAtCursor);
        const QList<QAction*> existing = menu->actions();
        if (existing.isEmpty()) {
            menu->addAction(jump);
        } else {
            menu->insertAction(existing.first(), jump);
            menu->insertSeparator(existing.first());
        }
    }
    menu->exec(event->globalPos());
    delete menu;
}

void CodeEditor::keyPressEvent(QKeyEvent* event) {
    if (consumeCompletionPopupKey(event))
        return;

    //Ctrl pressed: re-evaluate the jump affordance at the resting
    //pointer. The popup routing above stays first so that pressing Ctrl
    //with the completion popup open still dismisses it.
    if (event->key() == Qt::Key_Control) {
        updateLinkAffordance(m_lastHoverPos, true);
        return;   //a modifier key types nothing for the base class
    }

    //F12 and F6 both go to the definition at the cursor.
    if ((event->key() == Qt::Key_F12 || event->key() == Qt::Key_F6)
        && m_symbolIndex
        && goToDefinitionAt(textCursor())) {
        return;
    }

    QPlainTextEdit::keyPressEvent(event);

    if (event->key() == Qt::Key_Period)
        triggerPackageCompletion();
}

//The popup owns the keyboard while it is visible. Navigation keys are
//forwarded to it; identifier characters and Backspace edit the document
//and narrow the filter; anything else dismisses it. False means the
//key was not consumed — either the popup just closed on it (so it still
//reaches the editor below) or there was no popup to begin with.
bool CodeEditor::consumeCompletionPopupKey(QKeyEvent* event) {
    if (!m_completionPopup || !m_completionPopup->isVisible())
        return false;
    const QString typed = event->text();
    const QChar ch = typed.isEmpty() ? QChar() : typed.at(0);
    //Identifier characters keep the popup open and narrow it (the
    //typed text stays in the document); Backspace widens it again.
    const bool identChar =
        !typed.isEmpty() && (ch.isLetterOrNumber() || ch == '_');
    switch (event->key()) {
    case Qt::Key_Escape:
        closeCompletion();
        return true;
    case Qt::Key_Down:
    case Qt::Key_Up:
    case Qt::Key_Enter:
    case Qt::Key_Return:
    case Qt::Key_Tab:
        QCoreApplication::sendEvent(m_completionPopup, event);
        return true;
    default:
        if (identChar
            || (event->key() == Qt::Key_Backspace
                && !m_completionFilter.isEmpty())) {
            const QString filterBefore = m_completionFilter;
            QPlainTextEdit::keyPressEvent(event);  //edit the document
            if (event->key() == Qt::Key_Backspace)
                m_completionFilter = filterBefore.chopped(1);
            else
                m_completionFilter += ch;
            refilterCompletion();
            return true;
        }
        closeCompletion();  //anything else dismisses the popup
        return false;
    }
}

void CodeEditor::keyReleaseEvent(QKeyEvent* event) {
    //Ctrl released: the jump affordance must go even though the pointer
    //has not moved. X11 auto-repeat fakes release/press pairs while the
    //key is still held, so only a genuine release counts.
    if (event->key() == Qt::Key_Control && !event->isAutoRepeat()) {
        updateLinkAffordance(m_lastHoverPos, false);
        return;
    }
    QPlainTextEdit::keyReleaseEvent(event);
}

} // namespace nlang
