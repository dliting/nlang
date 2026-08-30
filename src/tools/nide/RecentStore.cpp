/*--- RecentStore.cpp - cross-session MRU of opened paths ---*/
#include "RecentStore.h"

#include <QFileInfo>
#include <QSettings>

namespace nlang {
namespace {
//House ceiling for the recent list.
const int DEFAULT_RECENT_CAPACITY = 10;
}

QString RecentStore::keyOf(const QString& path) {
    QString key = QFileInfo(path).absoluteFilePath();
#ifdef _WIN32
    key = key.toLower();
#endif
    return key;
}

void RecentStore::push(const QString& absolutePath) {
    const QString key = keyOf(absolutePath);
    for (int i = 0; i < m_entries.size(); ) {
        if (keyOf(m_entries.at(i)) == key)
            m_entries.removeAt(i);
        else
            ++i;
    }
    m_entries.prepend(QFileInfo(absolutePath).absoluteFilePath());
    while (m_entries.size() > DEFAULT_RECENT_CAPACITY)
        m_entries.removeLast();
}

void RecentStore::replace(const QString& oldPath, const QString& newPath) {
    const QString oldKey = keyOf(oldPath);
    for (int i = 0; i < m_entries.size(); ++i) {
        if (keyOf(m_entries.at(i)) == oldKey) {
            m_entries[i] = QFileInfo(newPath).absoluteFilePath();
            return;
        }
    }
}

void RecentStore::save(QSettings& settings) const {
    settings.setValue(QStringLiteral("recent/entries"), m_entries);
}

void RecentStore::load(QSettings& settings) {
    //toStringList() also maps a lone QString variant to a one-element
    //list, so a hand-edited store reads back sanely.
    m_entries =
        settings.value(QStringLiteral("recent/entries")).toStringList();
    while (m_entries.size() > DEFAULT_RECENT_CAPACITY)
        m_entries.removeLast();
}

} // namespace nlang
