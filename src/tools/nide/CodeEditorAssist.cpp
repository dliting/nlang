/*--- CodeEditorAssist.cpp - library-backed code assistance of the NLang IDE
    code editor: symbol-index wiring, hover help, the completion popup
    (including its menu-like dismissal) and F12 navigation. ---*/
#include "CodeEditor.h"

#include "nlang/langservice/SymbolIndex.h"

#include <QCoreApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QListWidget>
#include <QTextBlock>
#include <QToolTip>

namespace nlang {

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

void CodeEditor::mousePressEvent(QMouseEvent* event) {
    //A click in the editor repositions the cursor; the completion
    //popup would stay anchored at a stale position — close it. Focus
    //stays on the editor here, so focusOutEvent alone misses this path.
    closeCompletion();
    QPlainTextEdit::mousePressEvent(event);
}

void CodeEditor::focusOutEvent(QFocusEvent* event) {
    //The completion popup belongs to this editor's focus: when focus
    //moves elsewhere (another panel, a dialog, another window) the
    //popup must go with it — like a menu, not a persistent overlay.
    closeCompletion();
    QPlainTextEdit::focusOutEvent(event);
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
    //Qt::ToolTip keeps the frameless window above its parent without a
    //system-wide on-top hint; dismissal is driven by the editor (key /
    //click / focus-out), not by the window type.
    m_completionPopup->setWindowFlags(Qt::ToolTip);
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
