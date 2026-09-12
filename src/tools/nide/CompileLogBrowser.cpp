/*--- CompileLogBrowser.cpp - compiler output browser with error navigation ---*/
#include "CompileLogBrowser.h"

#include <QFileInfo>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextBlock>

namespace nlang {
namespace {

//ncc diagnostic shape: "<file>(line N, char N): <message>".
const QRegularExpression kLogLinePattern(
    "(.+)\\(line ([0-9]+), char ([0-9]+)\\): (.*)");

} // namespace

CompileLogBrowser::CompileLogBrowser(QWidget* parent)
    : QTextBrowser(parent)
{
}

bool CompileLogBrowser::parseLogLine(const QString& logLine,
                                     CompileLogItemInfo& info) {
    const QRegularExpressionMatch match = kLogLinePattern.match(logLine);
    if (!match.hasMatch())
        return false;

    info.filePath = match.captured(1);
    info.line = static_cast<size_t>(match.captured(2).toULongLong());
    info.column = static_cast<size_t>(match.captured(3).toULongLong());
    info.message = match.captured(4);
    return true;
}

void CompileLogBrowser::mouseDoubleClickEvent(QMouseEvent* event) {
    QTextBrowser::mouseDoubleClickEvent(event);
    //Only a left double-click navigates; other buttons keep the base
    //behavior.
    if (event->button() == Qt::LeftButton)
        emitLineUnderCursor();
}

void CompileLogBrowser::emitLineUnderCursor() {
    QTextCursor cursor = textCursor();
    cursor.select(QTextCursor::LineUnderCursor);
    setTextCursor(cursor);
    const QString lineText = cursor.block().text();
    if (lineText.isEmpty())
        return;   // blank line -- nothing to navigate to

    CompileLogItemInfo info;
    if (parseLogLine(lineText, info)) {
        //Resolve a relative source path against the project when one is
        //attached; absolute paths pass through unchanged.
        if (m_project != nullptr && QFileInfo(info.filePath).isRelative())
            info.filePath = m_project->absolutePathOf(info.filePath);
    } else {
        info.message = lineText;
    }
    emit lineSelected(info);
}

} // namespace nlang
