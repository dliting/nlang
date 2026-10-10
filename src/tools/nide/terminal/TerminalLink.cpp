/*---
    TerminalLink.cpp — "(file:line)" token detection for Ctrl+click.
---*/
#include "TerminalLink.h"

#include <QRegularExpression>

namespace nlang {
namespace terminal {
namespace {

//A path-ish token (no spaces/parens) directly followed by :digits or
//:?. Matches absolute (E:\x\a.n / /x/a.n) and bare (a.n) shapes. Free
//match: ordinary text like "12:30" links too — navigation without a
//real file lands on the status bar, never a crash (same liberal
//strategy as terminal emulators).
const QRegularExpression kFileLinePattern(
    "([^\\s()]+):([0-9]+|\\?)");

} // namespace

QVector<SourceLink> FindSourceLinks(const QString& rowText) {
    QVector<SourceLink> links;
    QRegularExpressionMatchIterator it =
        kFileLinePattern.globalMatch(rowText);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        SourceLink link;
        link.filePath = m.captured(1);
        const QString lineText = m.captured(2);
        link.line = lineText == QLatin1String("?")
            ? 0 : lineText.toInt();
        link.firstChar = m.capturedStart();
        link.lastChar = m.capturedEnd() - 1;
        links.append(link);
    }
    return links;
}

} // namespace terminal
} // namespace nlang
