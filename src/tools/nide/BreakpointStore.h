/*--- BreakpointStore.h - cross-session breakpoint table ---*/
#ifndef NLANG_TOOLS_NIDE_BREAKPOINT_STORE_H
#define NLANG_TOOLS_NIDE_BREAKPOINT_STORE_H

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

class QSettings;

namespace nlang {

//Breakpoints keyed by absolute source path (line sets). The same
//case-folding rule as RecentStore/editorKey: paths fold case on Windows
//only. No UI and no debug-session knowledge: MainWindow persists the
//table, paints it through the editors and mirrors it into ndb.
class BreakpointStore {
public:
    //Flip one (path, line); true = the line now carries a breakpoint.
    bool toggle(const QString& absolutePath, int line);
    bool contains(const QString& absolutePath, int line) const;
    //The breakpoint lines of one file (empty when none).
    QSet<int> linesOf(const QString& absolutePath) const;
    //Files that carry at least one breakpoint.
    QStringList files() const;
    //Follow a file rename so the breakpoints survive (no rank to keep).
    void rename(const QString& oldPath, const QString& newPath);
    //Single key "breakpoints/entries": one "<path>\t<line>,<line>,..."
    //entry per file; cheap enough for write-through on every toggle.
    void save(QSettings& settings) const;
    void load(QSettings& settings);
    //The fold rule as a public helper: MainWindow keys its wire-id map
    //with it, so receipts echoed by ndb and editor paths land on the
    //same key. Same rule as RecentStore/editorKey (lowercase on Windows).
    static QString normalizedKey(const QString& path);

private:
    QMap<QString, QSet<int>> m_linesByFile;  //normalized key -> lines
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_BREAKPOINT_STORE_H
