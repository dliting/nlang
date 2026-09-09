/*--- BreakpointStore.cpp - cross-session breakpoint table ---*/
#include "BreakpointStore.h"

#include <QFileInfo>
#include <QSettings>

namespace nlang {
namespace {
//QSettings key holding the per-file breakpoint entries.
const char* const BREAKPOINT_ENTRIES_KEY = "breakpoints/entries";
//Entry layout: "<absolutePath>\t<line>,<line>,..." -- a tab cannot occur
//in a path, so the split stays unambiguous.
const QChar ENTRY_SEPARATOR = QLatin1Char('\t');
const QChar LINE_SEPARATOR = QLatin1Char(',');
}

QString BreakpointStore::normalizedKey(const QString& path) {
    QString key = QFileInfo(path).absoluteFilePath();
#ifdef _WIN32
    key = key.toLower();
#endif
    return key;
}

bool BreakpointStore::toggle(const QString& absolutePath, int line) {
    QSet<int>& lines = m_linesByFile[normalizedKey(absolutePath)];
    if (lines.contains(line)) {
        lines.remove(line);
        if (lines.isEmpty())
            m_linesByFile.remove(normalizedKey(absolutePath));
        return false;
    }
    lines.insert(line);
    return true;
}

bool BreakpointStore::contains(const QString& absolutePath, int line) const {
    return m_linesByFile.value(normalizedKey(absolutePath)).contains(line);
}

QSet<int> BreakpointStore::linesOf(const QString& absolutePath) const {
    return m_linesByFile.value(normalizedKey(absolutePath));
}

QStringList BreakpointStore::files() const {
    QStringList paths;
    for (auto it = m_linesByFile.constBegin();
            it != m_linesByFile.constEnd(); ++it)
        paths << it.key();
    return paths;
}

void BreakpointStore::rename(const QString& oldPath, const QString& newPath) {
    const QString oldKey = normalizedKey(oldPath);
    const auto it = m_linesByFile.find(oldKey);
    if (it == m_linesByFile.end())
        return;
    m_linesByFile.insert(normalizedKey(newPath), it.value());
    m_linesByFile.erase(it);
}

void BreakpointStore::save(QSettings& settings) const {
    QStringList entries;
    for (auto it = m_linesByFile.constBegin();
            it != m_linesByFile.constEnd(); ++it) {
        QStringList lines;
        for (int line : it.value())
            lines << QString::number(line);
        entries << it.key() + ENTRY_SEPARATOR + lines.join(LINE_SEPARATOR);
    }
    settings.setValue(BREAKPOINT_ENTRIES_KEY, entries);
}

void BreakpointStore::load(QSettings& settings) {
    m_linesByFile.clear();
    //toStringList() also maps a lone QString variant to a one-element
    //list, so a hand-edited store reads back sanely.
    const QStringList entries =
        settings.value(BREAKPOINT_ENTRIES_KEY).toStringList();
    for (const QString& entry : entries) {
        const int separator = entry.indexOf(ENTRY_SEPARATOR);
        if (separator <= 0)
            continue;   //no path or no line half: skip the broken entry
        QSet<int> lines;
        for (const QString& line :
                entry.mid(separator + 1).split(LINE_SEPARATOR,
                                               Qt::KeepEmptyParts)) {
            bool ok = false;
            const int value = line.toInt(&ok);
            if (ok && value > 0)
                lines.insert(value);
        }
        if (!lines.isEmpty())
            //Normalize like every other write path: a hand-edited store
            //may spell the path in any case, and an unnormalized key
            //could never be matched (or toggled away) again.
            m_linesByFile.insert(normalizedKey(entry.left(separator)),
                                 lines);
    }
}

} // namespace nlang
