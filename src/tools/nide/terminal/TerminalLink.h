/*---
    TerminalLink.h — source links inside terminal output rows: the
    "(file:line)" family parsed from row text (Ctrl+click navigation
    targets for run-error backtraces). Pure parsing — no widget state.
---*/
#pragma once
#include <QString>
#include <QVector>

namespace nlang {
namespace terminal {

//One clickable source reference on a terminal row. firstChar/lastChar
//are inclusive QString positions of the whole "file:line" token; the
//widget maps them to grid columns through its per-cell walk. line 0 is
//the `?` form — navigation refuses it.
struct SourceLink {
    QString filePath;
    int line = 0;
    int firstChar = 0;
    int lastChar = 0;
    bool isValid() const { return !filePath.isEmpty(); }
};

//All links on the row, left-to-right (empty when none).
QVector<SourceLink> FindSourceLinks(const QString& rowText);

} // namespace terminal
} // namespace nlang
