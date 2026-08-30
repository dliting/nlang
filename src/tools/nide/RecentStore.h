/*--- RecentStore.h - cross-session MRU of opened paths ---*/
#ifndef NLANG_TOOLS_NIDE_RECENT_STORE_H
#define NLANG_TOOLS_NIDE_RECENT_STORE_H

#include <QStringList>

class QSettings;

namespace nlang {

//Ordered MRU of absolute paths, most recent first. Dedup and in-list
//checks fold case on Windows only -- the same guard FileEditor's
//editorKey uses. No UI: MainWindow owns the menu.
class RecentStore {
public:
    //Prepend, drop any earlier spelling of the same path, trim to the
    //capacity ceiling.
    void push(const QString& absolutePath);
    //Swap an in-list old path for its new spelling in place (MRU rank
    //kept); a no-op when the old path is not listed -- a rename is not
    //an open.
    void replace(const QString& oldPath, const QString& newPath);
    QStringList entries() const { return m_entries; }
    void clear() { m_entries.clear(); }
    //Single key "recent/entries"; cheap enough for write-through.
    void save(QSettings& settings) const;
    void load(QSettings& settings);

private:
    static QString keyOf(const QString& path);
    QStringList m_entries;  //verbatim absolute paths, most recent first
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_RECENT_STORE_H
