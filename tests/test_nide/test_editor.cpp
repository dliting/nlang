/*--- test_editor.cpp - FileEditor/CodeEditor/EditorManager unit tests ---*/
#include "../../../src/tools/nide/CodeEditor.h"
#include "../../../src/tools/nide/FileEditor.h"

#include <QDir>
#include <QFile>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

using namespace nlang;

class TestEditor : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmpDir;

    // Absolute path of a file under the temp dir.
    QString abs(const QString& relPath) { return m_tmpDir.path() + "/" + relPath; }

    // The QPlainTextEdit behind a CodeFileEditor (content is ASCII-only
    // in these tests, so QTextStream's default codec is fine here).
    QPlainTextEdit* editOf(FileEditor* editor) {
        return qobject_cast<QPlainTextEdit*>(editor->widget());
    }

    //Empty string on failure (the caller's QCOMPARE then fails visibly).
    QString readAllText(const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return QString();
        return QTextStream(&f).readAll();
    }

    void writeAllText(const QString& path, const QString& content) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QTextStream(&f) << content;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_tmpDir.isValid());
    }

    // --- EditorManager: open/create lifecycle ---

    void testOpenMissingFileReturnsNull() {
        EditorManager manager;
        QVERIFY(manager.open(abs("missing.n")) == nullptr);
        QVERIFY2(!manager.lastError().isEmpty(),
                 "failed open must report a reason");
    }

    void testCreateNewFile() {
        EditorManager manager;
        QString path = abs("fresh.n");
        FileEditor* editor = manager.openNew(path);
        QVERIFY(editor != nullptr);
        QVERIFY(QFile::exists(path));
        QVERIFY(!editor->dirty());
        QCOMPARE(manager.size(), size_t(1));
        QVERIFY(manager.find(path) == editor);
    }

    void testOpenNewExistingPathFails() {
        writeAllText(abs("there.n"), "int x = 1;\n");
        EditorManager manager;
        QVERIFY(manager.openNew(abs("there.n")) == nullptr);  // exists already
        QVERIFY(manager.size() == size_t(0));
        QVERIFY(!manager.lastError().isEmpty());
    }

    void testOpenNewAlreadyOpenFails() {
        EditorManager manager;
        FileEditor* editor = manager.openNew(abs("dup.n"));
        QVERIFY(editor != nullptr);

        //File removed on disk behind our back; openNew must fail cleanly
        //instead of silently replacing the open editor in the map.
        QVERIFY(QFile::remove(abs("dup.n")));
        QVERIFY(manager.openNew(abs("dup.n")) == nullptr);
        QVERIFY(manager.size() == size_t(1));
        QVERIFY(manager.find(abs("dup.n")) == editor);
    }

    void testOpenLoadsContent() {
        writeAllText(abs("hello.n"), "int a = 1;\nint b = 2;\n");
        EditorManager manager;
        QSignalSpy spy(&manager, &EditorManager::saveStateChanged);
        FileEditor* editor = manager.open(abs("hello.n"));
        QVERIFY(editor != nullptr);
        QCOMPARE(editOf(editor)->toPlainText(), QString("int a = 1;\nint b = 2;\n"));
        QVERIFY(!editor->dirty());
        QCOMPARE(editor->filePath(), abs("hello.n"));
        QCOMPARE(spy.count(), 0);  // programmatic load must not emit
    }

    void testOpenSaveUtf8RoundTrip() {
        //Raw bytes, not the locale-coded helpers: non-ASCII content is
        //what pins the explicit UTF-8 codecs (GBK would corrupt them).
        QByteArray raw = QByteArray("int x = 1; // \xE4\xB8\xAD\xE6\x96\x87\xE6\xB3\xA8\xE9\x87\x8A\n");
        {
            QFile f(abs("utf8.n"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(raw);
        }
        EditorManager manager;
        FileEditor* editor = manager.open(abs("utf8.n"));
        QVERIFY(editor != nullptr);
        QCOMPARE(editOf(editor)->toPlainText(), QString::fromUtf8(raw));

        editOf(editor)->setPlainText(QString::fromUtf8("// \xE4\xBF\xAE\xE6\x94\xB9\n"));
        QVERIFY(editor->save());
        QFile f(abs("utf8.n"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("// \xE4\xBF\xAE\xE6\x94\xB9\n"));
    }

    void testOpenTwiceReturnsSameEditor() {
        writeAllText(abs("once.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* first = manager.open(abs("once.n"));
        FileEditor* second = manager.open(abs("once.n"));
        QVERIFY(first != nullptr);
        QVERIFY(first == second);
        QCOMPARE(manager.size(), size_t(1));
    }

    void testOpenCaseFoldedPathSameEditor() {
        writeAllText(abs("case.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* first = manager.open(abs("case.n"));
#ifdef _WIN32
        //One physical file, two spellings: NTFS is case-insensitive, so
        //the manager key folds case (same rule as ProjectModel dedupKey).
        FileEditor* second = manager.open(abs("CASE.N"));
        QVERIFY(first == second);
        QCOMPARE(manager.size(), size_t(1));
#endif
    }

    // --- dirty tracking ---

    void testEditMarksDirty() {
        writeAllText(abs("dirty.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("dirty.n"));

        QSignalSpy spy(&manager, &EditorManager::saveStateChanged);
        editOf(editor)->setPlainText("int x = 2;\n");
        QVERIFY(editor->dirty());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<FileEditor*>(), editor);
    }

    void testDirtyTitleAsterisk() {
        writeAllText(abs("title.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("title.n"));
        QCOMPARE(editOf(editor)->windowTitle(), QString("title.n"));

        editOf(editor)->setPlainText("int x = 2;\n");
        QCOMPARE(editOf(editor)->windowTitle(), QString("title.n*"));

        QVERIFY(editor->save());
        QCOMPARE(editOf(editor)->windowTitle(), QString("title.n"));
    }

    // --- save ---

    void testSaveWritesDisk() {
        writeAllText(abs("save.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("save.n"));
        editOf(editor)->setPlainText("int x = 42;\n");
        QVERIFY(editor->dirty());

        QVERIFY(editor->save());
        QVERIFY(!editor->dirty());
        QCOMPARE(readAllText(abs("save.n")), QString("int x = 42;\n"));
    }

    void testSaveCleanIsNoop() {
        QString content = "int x = 7;\n";
        writeAllText(abs("clean.n"), content);
        EditorManager manager;
        FileEditor* editor = manager.open(abs("clean.n"));
        QVERIFY(!editor->dirty());

        QVERIFY(editor->save());  // clean: nothing to do
        QCOMPARE(readAllText(abs("clean.n")), content);
    }

    void testSaveMissingDirectoryFails() {
        EditorManager manager;
        FileEditor* editor = manager.openNew(abs("nowhere-src.n"));
        editOf(editor)->setPlainText("int x = 1;\n");

        // Relocate the editor to a path whose directory does not exist.
        QVERIFY(!editor->saveAs(abs("no_such_dir/moved.n")));
        QVERIFY(editor->dirty());
        QVERIFY(!editor->lastError().isEmpty());
        QVERIFY(manager.find(abs("nowhere-src.n")) == editor);  // rekey skipped
    }

    // --- saveAs: re-keying the manager (EN bug: map key went stale) ---

    void testSaveAsRekeysManager() {
        writeAllText(abs("orig.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("orig.n"));
        editOf(editor)->setPlainText("int x = 2;\n");

        QVERIFY(editor->saveAs(abs("renamed.n")));
        QVERIFY(!editor->dirty());
        QCOMPARE(editor->filePath(), abs("renamed.n"));
        // Old file untouched, new file has the edited content.
        QCOMPARE(readAllText(abs("orig.n")), QString("int x = 1;\n"));
        QCOMPARE(readAllText(abs("renamed.n")), QString("int x = 2;\n"));
        // The manager follows the rename.
        QVERIFY(manager.find(abs("renamed.n")) == editor);
        QVERIFY(manager.find(abs("orig.n")) == nullptr);
        QCOMPARE(manager.size(), size_t(1));
    }

    void testSaveAsSamePathJustSaves() {
        writeAllText(abs("same.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("same.n"));
        editOf(editor)->setPlainText("int x = 3;\n");

        QVERIFY(editor->saveAs(abs("same.n")));
        QCOMPARE(readAllText(abs("same.n")), QString("int x = 3;\n"));
        QVERIFY(manager.find(abs("same.n")) == editor);
    }

    void testSaveAsOverOpenEditorFails() {
        writeAllText(abs("a.n"), "int a = 1;\n");
        writeAllText(abs("b.n"), "int b = 2;\n");
        EditorManager manager;
        FileEditor* a = manager.open(abs("a.n"));
        FileEditor* b = manager.open(abs("b.n"));
        editOf(b)->setPlainText("int b = 3;\n");

        //Saving over another open editor's file would orphan that editor
        //inside the manager -- reject before anything is written.
        QVERIFY(!b->saveAs(abs("a.n")));
        QVERIFY(!b->lastError().isEmpty());
        QCOMPARE(readAllText(abs("a.n")), QString("int a = 1;\n"));  // untouched
        QVERIFY(b->dirty());
        QVERIFY(b->filePath() == abs("b.n"));
        QVERIFY(manager.find(abs("a.n")) == a);
        QVERIFY(manager.find(abs("b.n")) == b);
        QCOMPARE(manager.size(), size_t(2));
    }

    void testSaveAsCaseVariantPathRekeys() {
        writeAllText(abs("case2.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("case2.n"));
        editOf(editor)->setPlainText("int x = 2;\n");

#ifdef _WIN32
        //A case-variant spelling of the same file is a rename, not a
        //clash with itself: the new spelling is adopted and the map
        //keeps exactly one entry (rekey erases before inserting).
        QVERIFY(editor->saveAs(abs("CASE2.N")));
        QVERIFY(editor->filePath() == abs("CASE2.N"));
        QCOMPARE(manager.size(), size_t(1));
        QVERIFY(manager.find(abs("case2.n")) == editor);
        QVERIFY(manager.find(abs("CASE2.N")) == editor);
#else
        QVERIFY(editor->saveAs(abs("renamed2.n")));
        QVERIFY(manager.find(abs("renamed2.n")) == editor);
        QCOMPARE(manager.size(), size_t(1));
#endif
    }

    // --- onExternalRename: an external rename moved the file under us ---

    void testExternalRenameMovesOpenDirtyEditor() {
        writeAllText(abs("extorig.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("extorig.n"));
        editOf(editor)->setPlainText("int x = 2;\n");

        editor->onExternalRename(abs("extrenamed.n"));

        QCOMPARE(editor->filePath(), abs("extrenamed.n"));
        QVERIFY(editor->dirty());  // never writes nor clears the flag
        QCOMPARE(editOf(editor)->windowTitle(), QString("extrenamed.n*"));
        QVERIFY(manager.find(abs("extrenamed.n")) == editor);
        QVERIFY(manager.find(abs("extorig.n")) == nullptr);
        QCOMPARE(manager.size(), size_t(1));
        QCOMPARE(editOf(editor)->accessibleName(), abs("extrenamed.n"));
        //No write through the editor: the rename caller moves the disk
        //file itself (nothing to see here either way in this fixture).
        QVERIFY(!QFile::exists(abs("extrenamed.n")));
        QCOMPARE(readAllText(abs("extorig.n")), QString("int x = 1;\n"));
    }

    void testExternalRenameCaseVariantKeepsSingleEntry() {
        writeAllText(abs("case3.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("case3.n"));

#ifdef _WIN32
        //A case-variant spelling is still one physical file: rekey erases
        //before inserting, so the map keeps exactly one entry.
        editor->onExternalRename(abs("CASE3.N"));
        QVERIFY(manager.find(abs("case3.n")) == editor);
        QVERIFY(manager.find(abs("CASE3.N")) == editor);
#else
        editor->onExternalRename(abs("case3renamed.n"));
        QVERIFY(manager.find(abs("case3renamed.n")) == editor);
#endif
        QCOMPARE(manager.size(), size_t(1));
    }

    // --- remove/clear ---

    void testRemoveDeletesEditor() {
        writeAllText(abs("bye.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("bye.n"));
        QVERIFY(manager.remove(editor));
        QCOMPARE(manager.size(), size_t(0));
        QVERIFY(manager.find(abs("bye.n")) == nullptr);
    }

    void testRemoveDirtyEditorDiscardsChanges() {
        writeAllText(abs("discard.n"), "int x = 1;\n");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("discard.n"));
        editOf(editor)->setPlainText("int x = 999;\n");
        QVERIFY(editor->dirty());

        //The save-before-close prompt belongs to the UI layer: remove()
        //closes unconditionally and the disk keeps the last saved state.
        QVERIFY(manager.remove(editor));
        QCOMPARE(manager.size(), size_t(0));
        QCOMPARE(readAllText(abs("discard.n")), QString("int x = 1;\n"));
    }

    void testRemoveUnknownEditorReturnsFalse() {
        writeAllText(abs("stay.n"), "int x = 1;\n");
        EditorManager manager;
        EditorManager other;
        FileEditor* editor = other.open(abs("stay.n"));
        QVERIFY(!manager.remove(editor));  // not owned by this manager
        QCOMPARE(manager.size(), size_t(0));
        QCOMPARE(other.size(), size_t(1));
    }

    void testClear() {
        writeAllText(abs("c1.n"), "int x = 1;\n");
        writeAllText(abs("c2.n"), "int y = 2;\n");
        EditorManager manager;
        manager.open(abs("c1.n"));
        manager.open(abs("c2.n"));
        QCOMPARE(manager.size(), size_t(2));

        manager.clear();
        QCOMPARE(manager.size(), size_t(0));
    }

    // --- position info ---

    void testPositionInfo() {
        //No trailing newline: "End" must land on block 2, not a phantom
        //third block.
        writeAllText(abs("pos.n"), "int a = 1;\nint b = 2;");
        EditorManager manager;
        FileEditor* editor = manager.open(abs("pos.n"));
        QCOMPARE(editor->positionInfo(), QString("line: 1\tcharacter: 1"));

        // Move the cursor to the end of the second line.
        QPlainTextEdit* edit = editOf(editor);
        QTextCursor cursor = edit->textCursor();
        cursor.movePosition(QTextCursor::End);
        edit->setTextCursor(cursor);
        QCOMPARE(editor->positionInfo(), QString("line: 2\tcharacter: 11"));
    }

    // --- CodeEditor specifics ---

    void testLineAreaWidthGrowsWithDigits() {
        CodeEditor editor;
        editor.setPlainText("one line");  // 1 block -> 1 digit
        QCOMPARE(editor.lineAreaWidth(),
                 8 * 2 + editor.fontMetrics().horizontalAdvance('9') * 1);

        QStringList lines;
        for (int i = 0; i < 12; ++i)
            lines << QString("line %1").arg(i);
        editor.setPlainText(lines.join('\n'));  // 12 blocks -> 2 digits
        QCOMPARE(editor.lineAreaWidth(),
                 8 * 2 + editor.fontMetrics().horizontalAdvance('9') * 2);
    }

    void testLineAreaGeometryFollowsResize() {
        CodeEditor editor;
        //Hidden widgets defer resize events (WA_PendingResizeEvent) --
        //show() delivers them.
        editor.resize(400, 300);
        editor.show();
        QApplication::processEvents();
        LineArea* area = editor.lineArea();
        QVERIFY(area != nullptr);
        QCOMPARE(area->width(), editor.lineAreaWidth());
        QCOMPARE(area->height(), 300);
        QCOMPARE(area->pos(), QPoint(0, 0));  // NoFrame: flush top-left
    }

    void testHighlightCurrentLine() {
        CodeEditor editor;
        editor.setPlainText("a\nb");
        QCOMPARE(editor.extraSelections().size(), 1);  // current line

        editor.setReadOnly(true);
        // Re-trigger via cursor move (the highlight slot skips read-only).
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
        QCOMPARE(editor.extraSelections().size(), 0);
        editor.setReadOnly(false);
    }

    void testTabStopConfigured() {
        CodeEditor editor;
        QCOMPARE(editor.tabStopDistance(),
                 4.0 * editor.fontMetrics().horizontalAdvance(' '));
    }
};

QTEST_MAIN(TestEditor)
#include "test_editor.moc"
