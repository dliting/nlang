/*--- RunErrorBrowser.cpp - finished-run output browser with error navigation ---*/
#include "RunErrorBrowser.h"

#include <QMouseEvent>
#include <QRegularExpression>
#include <QTextCursor>

namespace nlang {
namespace {

//nvm's failure report: "Runtime error: <msg>" then "Backtrace:" with
//"  at <func> (<file>:<line>)" rows (line is `?` when the artifact was
//built --no-debug-info).
const QRegularExpression kRuntimeErrorPattern(
    "^Runtime error: (.*)$");
const QRegularExpression kFramePattern(
    "^\\s*at \\S+ \\((.+):([0-9]+|\\?)\\)$");

} // namespace

RunErrorBrowser::RunErrorBrowser(QWidget* parent)
    : QTextBrowser(parent)
{
}

void RunErrorBrowser::setRunOutput(const QString& text) {
    clear();
    m_items.clear();
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        RunErrorItemInfo info;
        if (!parseRunLine(line, info))
            continue;
        m_items.append(info);
        append(line);
    }
}

bool RunErrorBrowser::parseRunLine(const QString& line,
                                   RunErrorItemInfo& info) {
    const QRegularExpressionMatch errMatch =
        kRuntimeErrorPattern.match(line);
    if (errMatch.hasMatch()) {
        info.message = line;
        return true;   //message row: no site
    }
    const QRegularExpressionMatch frameMatch = kFramePattern.match(line);
    if (!frameMatch.hasMatch())
        return false;
    info.filePath = frameMatch.captured(1);
    const QString lineText = frameMatch.captured(2);
    info.line = lineText == QLatin1String("?")
        ? 0 : lineText.toInt();
    info.message = line;
    return true;
}

void RunErrorBrowser::mouseDoubleClickEvent(QMouseEvent* event) {
    QTextBrowser::mouseDoubleClickEvent(event);
    //Only a left double-click navigates; other buttons keep the base
    //behavior.
    if (event->button() == Qt::LeftButton)
        emitItemUnderCursor();
}

void RunErrorBrowser::emitItemUnderCursor() {
    QTextCursor cursor = textCursor();
    cursor.select(QTextCursor::LineUnderCursor);
    setTextCursor(cursor);
    //One appended block per kept item, in order.
    const int blockNumber = cursor.blockNumber();
    if (blockNumber < 0 || blockNumber >= m_items.size())
        return;
    emit itemSelected(m_items[blockNumber]);
}

} // namespace nlang
