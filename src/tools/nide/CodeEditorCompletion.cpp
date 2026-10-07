/*--- CodeEditorCompletion.cpp - the package completion popup of the NLang
    IDE code editor: the candidate list opened after a package dot, its
    typed-prefix filtering and the acceptance of a candidate. ---*/
#include "CodeEditor.h"

#include "nlang/langservice/SymbolIndex.h"

#include <QListWidget>
#include <QTextBlock>

namespace nlang {

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
    //A child of the viewport, not a Qt::ToolTip top-level: a ToolTip
    //window swallows mouse presses, so candidates could never be
    //clicked. As an in-editor child the list takes clicks while the
    //editor keeps the keyboard focus (typing filters the list).
    m_completionPopup = new QListWidget(viewport());
    m_completionPopup->setFocusPolicy(Qt::NoFocus);
    m_completionCandidates = candidates;
    m_completionFilter.clear();
    m_completionAnchor = textCursor().position();
    for (const langservice::SymbolInfo* symbol : m_completionCandidates) {
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

    //Position the popup just below the cursor, in viewport coordinates.
    const QPoint pos = cursorRect().bottomLeft() + QPoint(0, 2);
    m_completionPopup->setGeometry(
        pos.x(), pos.y(), kCompletionPopupWidth,
        std::min(kCompletionPopupMaxHeight,
                 kCompletionItemHeight
                     * static_cast<int>(candidates.size()) + 6));
    m_completionPopup->setCurrentRow(0);
    m_completionPopup->raise();
    m_completionPopup->show();
}

void CodeEditor::refilterCompletion() {
    if (!m_completionPopup)
        return;
    m_completionPopup->clear();
    int matches = 0;
    const QString filter = m_completionFilter;
    for (const langservice::SymbolInfo* symbol : m_completionCandidates) {
        if (!QString::fromStdString(symbol->name).startsWith(filter))
            continue;
        auto* item = new QListWidgetItem(
            QString::fromStdString(symbol->name), m_completionPopup);
        item->setData(Qt::UserRole,
                      QString::fromStdString(symbol->pkg) + QLatin1Char('.')
                      + QString::fromStdString(symbol->name));
        ++matches;
    }
    if (matches == 0) {
        closeCompletion();  //nothing matches: stop suggesting
        return;
    }
    m_completionPopup->setGeometry(
        m_completionPopup->x(), m_completionPopup->y(),
        kCompletionPopupWidth,
        std::min(kCompletionPopupMaxHeight,
                 kCompletionItemHeight * matches + 6));
    m_completionPopup->setCurrentRow(0);
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
    //Replace everything typed since the trigger '.' (the filter text)
    //with the candidate's name; the package prefix is already typed.
    QTextCursor cursor = textCursor();
    const int end = cursor.position();
    if (m_completionAnchor >= 0 && end > m_completionAnchor) {
        cursor.setPosition(m_completionAnchor, QTextCursor::MoveAnchor);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        setTextCursor(cursor);
    }
    insertPlainText(item->data(Qt::UserRole).toString().section(
        QLatin1Char('.'), -1));
    closeCompletion();
}

void CodeEditor::closeCompletion() {
    if (m_completionPopup) {
        m_completionPopup->hide();
        m_completionPopup->deleteLater();
        m_completionPopup = nullptr;
    }
    m_completionCandidates.clear();
    m_completionFilter.clear();
    m_completionAnchor = -1;
}

} // namespace nlang
