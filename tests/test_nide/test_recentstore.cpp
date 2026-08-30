/*--- test_recentstore.cpp - RecentStore unit tests ---*/
#include "RecentStore.h"

#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using nlang::RecentStore;

class TestRecentStore : public QObject {
    Q_OBJECT

private slots:
    void pushPutsMostRecentFirst();
    void pushDedupesAnExistingSpelling();
    void pushTrimsToTheCapacityCeiling();
    void replaceKeepsRankAndOnlyWhenListed();
    void clearEmpties();
    void saveLoadRoundTrips();
    void loadReadsALoneStringEntry();

private:
    //A fresh ini per test function: no cross-test bleed.
    QString iniPath() const {
        return m_dir.filePath(
            QString::fromLatin1(QTest::currentTestFunction()) + ".ini");
    }
    QTemporaryDir m_dir;
};

void TestRecentStore::pushPutsMostRecentFirst() {
    RecentStore store;
    const QString a = m_dir.filePath("a.n");
    const QString b = m_dir.filePath("b.n");
    store.push(a);
    store.push(b);
    QCOMPARE(store.entries(), QStringList({b, a}));
}

void TestRecentStore::pushDedupesAnExistingSpelling() {
    RecentStore store;
    const QString lower = m_dir.filePath("main.n").toLower();
    const QString mixed = m_dir.filePath("Main.n");
    store.push(lower);
    store.push(mixed);
#ifdef _WIN32
    //One physical file (case folded): the NEW spelling survives.
    QCOMPARE(store.entries(), QStringList({mixed}));
#else
    QCOMPARE(store.entries(), QStringList({mixed, lower}));
#endif
}

void TestRecentStore::pushTrimsToTheCapacityCeiling() {
    RecentStore store;
    QStringList pushed;
    for (int i = 0; i < 12; ++i) {
        const QString path = m_dir.filePath(QString("f%1.n").arg(i));
        pushed << path;
        store.push(path);
    }
    QCOMPARE(store.entries().size(), 10);
    //The oldest two fell off; the newest is on top.
    QCOMPARE(store.entries().first(), pushed.last());
    QVERIFY(!store.entries().contains(pushed.first()));
    QVERIFY(!store.entries().contains(pushed.at(1)));
}

void TestRecentStore::replaceKeepsRankAndOnlyWhenListed() {
    RecentStore store;
    const QString a = m_dir.filePath("a.n");
    const QString b = m_dir.filePath("b.n");
    const QString a2 = m_dir.filePath("a2.n");
    store.push(a);
    store.push(b);  // entries: {b, a}
    store.replace(a, a2);
    QCOMPARE(store.entries(), QStringList({b, a2}));  //rank kept
    //A rename of something never opened is NOT an open: no new entry.
    const QString stranger = m_dir.filePath("stranger.n");
    const QString stranger2 = m_dir.filePath("stranger2.n");
    store.replace(stranger, stranger2);
    QCOMPARE(store.entries(), QStringList({b, a2}));
}

void TestRecentStore::clearEmpties() {
    RecentStore store;
    store.push(m_dir.filePath("a.n"));
    store.clear();
    QVERIFY(store.entries().isEmpty());
}

void TestRecentStore::saveLoadRoundTrips() {
    {
        RecentStore store;
        store.push(m_dir.filePath("a.n"));
        store.push(m_dir.filePath("b.n"));
        QSettings settings(iniPath(), QSettings::IniFormat);
        store.save(settings);
    }
    RecentStore loaded;
    QSettings settings(iniPath(), QSettings::IniFormat);
    loaded.load(settings);
    QCOMPARE(loaded.entries().size(), 2);
    QVERIFY(loaded.entries().first().endsWith("b.n"));
    QVERIFY(loaded.entries().last().endsWith("a.n"));
}

void TestRecentStore::loadReadsALoneStringEntry() {
    //A hand-edited store (or a single-entry one) may hold a lone
    //QString: value().toStringList() maps it to a one-element list.
    QSettings settings(iniPath(), QSettings::IniFormat);
    settings.setValue("recent/entries", m_dir.filePath("only.n"));
    RecentStore store;
    store.load(settings);
    QCOMPARE(store.entries().size(), 1);
    QVERIFY(store.entries().first().endsWith("only.n"));
}

QTEST_GUILESS_MAIN(TestRecentStore)
#include "test_recentstore.moc"
