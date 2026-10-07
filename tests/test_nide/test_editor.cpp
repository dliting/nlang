/*--- test_editor.cpp - FileEditor/CodeEditor/EditorManager/BreakpointStore
unit tests ---*/
#include "../../../src/tools/nide/BreakpointStore.h"
#include "../../../src/tools/nide/CodeEditor.h"
#include "../../../src/tools/nide/FileEditor.h"
#include "nlang/langservice/SymbolIndex.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextStream>
#include <QTimer>
#include <QtTest>

using namespace nlang;

class TestEditor : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmpDir;

    // Absolute path of a file under the temp dir.
    QString abs(const QString& relPath) { return m_tmpDir.path() + "/" + relPath; }

    // A fresh ini per test function: no cross-test bleed.
    QString iniPath() const {
        return m_tmpDir.filePath(
            QString::fromLatin1(QTest::currentTestFunction()) + ".ini");
    }

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

    // --- saveAs: re-keying the manager (stale map key hazard) ---

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
        //The leading breakpoint column joins the margin + digits sum.
        QCOMPARE(editor.lineAreaWidth(),
                 CodeEditor::kBreakpointColumnWidth + 8 * 2
                     + editor.fontMetrics().horizontalAdvance('9') * 1);

        QStringList lines;
        for (int i = 0; i < 12; ++i)
            lines << QString("line %1").arg(i);
        editor.setPlainText(lines.join('\n'));  // 12 blocks -> 2 digits
        QCOMPARE(editor.lineAreaWidth(),
                 CodeEditor::kBreakpointColumnWidth + 8 * 2
                     + editor.fontMetrics().horizontalAdvance('9') * 2);
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

    // --- qualified name extraction (hover / F12 target) ---

    void testQualifiedNameAt() {
        // "io.print(": columns 0:i 1:o 2:. 3-7:print 8:(
        const QString line = QStringLiteral("io.print(");
        for (int col = 3; col <= 8; ++col)
            QCOMPARE(CodeEditor::qualifiedNameAt(line, col),
                     QStringLiteral("io.print"));
        // Cursor on the namespace, the dot, or before it: no call target.
        for (int col = 0; col <= 2; ++col)
            QCOMPARE(CodeEditor::qualifiedNameAt(line, col), QString());

        QCOMPARE(CodeEditor::qualifiedNameAt("math.atan2", 6),
                 QStringLiteral("math.atan2"));
        // Embedded in an assignment / call.
        QCOMPARE(CodeEditor::qualifiedNameAt("int x = io.print(1)", 12),
                 QStringLiteral("io.print"));
        // No qualifier.
        QCOMPARE(CodeEditor::qualifiedNameAt("print", 3), QString());
        // Dotted-package chains are library-call shapes now
        // (a.b.c = package a.b + member c, exactly vendor.graphics.hue's
        // shape); object member chains yield the same text and simply
        // miss the index lookup.
        QCOMPARE(CodeEditor::qualifiedNameAt("a.b.c", 5),
                 QStringLiteral("a.b.c"));
        // Whitespace / punctuation column away from an identifier.
        QCOMPARE(CodeEditor::qualifiedNameAt("io. print", 3), QString());
    }

    // --- import package extraction (import go-to-definition target) ---

    void testImportPackageAt() {
        // "import io;": 0-5 keyword, 6 space, 7-8 ident, 9 ';'.
        QCOMPARE(CodeEditor::importPackageAt("import io;", 7),
                 QStringLiteral("io"));
        QCOMPARE(CodeEditor::importPackageAt("import io;", 8),
                 QStringLiteral("io"));
        // Boundary right after the ident still jumps; ';' and the
        // keyword itself do not.
        QCOMPARE(CodeEditor::importPackageAt("import io;", 9),
                 QStringLiteral("io"));
        QCOMPARE(CodeEditor::importPackageAt("import io;", 10), QString());
        QCOMPARE(CodeEditor::importPackageAt("import io;", 3), QString());
        // Dotted chain: any segment's column yields the whole chain
        // (the package, not a member call).
        QCOMPARE(CodeEditor::importPackageAt("import vendor.graphics;", 8),
                 QStringLiteral("vendor.graphics"));
        QCOMPARE(CodeEditor::importPackageAt("import vendor.graphics;", 15),
                 QStringLiteral("vendor.graphics"));
        // Leading whitespace and a missing semicolon are both fine.
        QCOMPARE(CodeEditor::importPackageAt("    import gfx.color", 15),
                 QStringLiteral("gfx.color"));
        // Not an import line / keyword-prefixed word / empty chain.
        QCOMPARE(CodeEditor::importPackageAt("io.print(1)", 4), QString());
        QCOMPARE(CodeEditor::importPackageAt("imports io;", 9), QString());
        QCOMPARE(CodeEditor::importPackageAt("import ;", 7), QString());
        // The ".*" wildcard is not part of the chain: the name is the
        // package ("utils", never "utils."), and a column on a chain
        // segment still resolves it.
        QCOMPARE(CodeEditor::importPackageAt("import utils.*;", 8),
                 QStringLiteral("utils"));
        int start = -1;
        int length = -1;
        QCOMPARE(CodeEditor::importPackageAt("import vendor.gfx.*;", 15,
                                             &start, &length),
                 QStringLiteral("vendor.gfx"));
        QCOMPARE(start, 7);
        QCOMPARE(length, 10);
    }

    // --- Ctrl+hover link decoration / context menu (go-to-definition) ---

    //Index fixture shared by the assist tests: io.n beside the temp dir
    //is the package "io" (three members so completion filtering has a
    //list to narrow).
    void writeIoPackage() {
        QFile f(abs("io.n"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("native void print(string s);\n"
                "native void println(string s);\n"
                "native void readInt();\n");
    }

    void testCtrlHoverLinkDecoration() {
        writeIoPackage();
        langservice::SymbolIndex index;
        index.LoadFile(abs("io.n").toStdString());

        CodeEditor editor;
        editor.setSymbolIndex(&index);
        editor.setPlainText("import io;\npublic int main() { return 0; }\n");

        //Ctrl+hover over "io": the token gains a blue-underlined
        //hyperlink selection covering exactly the package name.
        const QTextBlock importBlock = editor.document()->firstBlock();
        const int ioStart = importBlock.text().indexOf("io");
        QTextCursor atIo(importBlock);
        atIo.setPosition(importBlock.position() + ioStart + 1);
        const QPointF hoverPos(editor.cursorRect(atIo).center());

        auto moveEvent = [&editor, &hoverPos](Qt::KeyboardModifiers mods) {
            QMouseEvent move(QEvent::MouseMove, hoverPos, hoverPos,
                             hoverPos, Qt::NoButton, Qt::NoButton, mods);
            QCoreApplication::sendEvent(editor.viewport(), &move);
        };
        auto ctrlEvent = [&editor](QEvent::Type type,
                                   Qt::KeyboardModifiers mods) {
            QKeyEvent key(type, Qt::Key_Control, mods);
            QCoreApplication::sendEvent(&editor, &key);
        };
        //The blue-underlined link span, or a null cursor when plain.
        auto linkSpan = [&editor]() {
            for (const QTextEdit::ExtraSelection& sel :
                 editor.extraSelections()) {
                if (sel.format.fontUnderline()
                    && sel.format.foreground().color() == QColor(Qt::blue)
                    && sel.cursor.hasSelection())
                    return sel.cursor;
            }
            return QTextCursor();
        };

        //Moving with Ctrl held decorates the token as a link.
        moveEvent(Qt::ControlModifier);
        QVERIFY2(linkSpan().hasSelection(),
                 "Ctrl+hover must decorate the token as a link");
        QCOMPARE(linkSpan().selectionStart(),
                 importBlock.position() + ioStart);
        QCOMPARE(linkSpan().selectionEnd(),
                 importBlock.position() + ioStart + 2);
        QCOMPARE(editor.viewport()->cursor().shape(),
                 Qt::PointingHandCursor);

        //A move without Ctrl clears the decoration.
        moveEvent(Qt::NoModifier);
        QVERIFY(!linkSpan().hasSelection());

        //Pressing Ctrl while the pointer sits still must show the
        //affordance too (the modifier change alone refreshes it)...
        ctrlEvent(QEvent::KeyPress, Qt::ControlModifier);
        QVERIFY2(linkSpan().hasSelection(),
                 "Ctrl press alone must decorate the hovered token");
        QCOMPARE(editor.viewport()->cursor().shape(),
                 Qt::PointingHandCursor);

        //...and releasing it (still no mouse move) must revert at once.
        ctrlEvent(QEvent::KeyRelease, Qt::NoModifier);
        QVERIFY(!linkSpan().hasSelection());
        QCOMPARE(editor.viewport()->cursor().shape(),
                 Qt::IBeamCursor);
    }

    void testContextMenuOffersGoToDefinition() {
        writeIoPackage();
        langservice::SymbolIndex index;
        index.LoadFile(abs("io.n").toStdString());

        CodeEditor editor;
        editor.setSymbolIndex(&index);
        editor.setPlainText("import io;\npublic int main() { return 0; }\n");

        //A synthetic QMenu::exec may register as neither active popup
        //nor active modal (see test_mainwindow's activeMenu): scan the
        //visible top-level menus as the fallback.
        auto activeMenu = []() -> QMenu* {
            if (QMenu* menu =
                    qobject_cast<QMenu*>(QApplication::activePopupWidget()))
                return menu;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (QMenu* menu = qobject_cast<QMenu*>(widget)) {
                    if (menu->isVisible())
                        return menu;
                }
            }
            return nullptr;
        };
        //Open the context menu, snapshot its actions (the handler deletes
        //the menu on return) and close it inside the exec loop. The event
        //goes to the viewport: that is the widget under the mouse in a
        //real right-click, and QAbstractScrollArea's viewport filter
        //routes it back into the editor's contextMenuEvent.
        auto menuTexts = [&]() {
            QStringList texts;
            QTimer::singleShot(0, [&texts, &activeMenu]() {
                if (QMenu* menu = activeMenu()) {
                    for (QAction* action : menu->actions())
                        texts << action->text();
                    menu->close();
                }
            });
            QContextMenuEvent open(QContextMenuEvent::Mouse, QPoint(5, 5),
                                   QPoint(100, 100));
            QCoreApplication::sendEvent(editor.viewport(), &open);
            return texts;
        };

        //Cursor on the import package: the menu heads with the jump.
        QTextCursor atImport(editor.document()->firstBlock());
        atImport.setPosition(atImport.block().position()
                             + atImport.block().text().indexOf("io") + 1);
        editor.setTextCursor(atImport);
        const QStringList withTarget = menuTexts();
        QVERIFY2(withTarget.contains(CodeEditor::tr("Go to Definition")),
                 qPrintable(withTarget.join(QStringLiteral(" | "))));

        //Cursor on plain text: no jump entry.
        QTextCursor atEnd(editor.document());
        atEnd.movePosition(QTextCursor::End);
        editor.setTextCursor(atEnd);
        QVERIFY(!menuTexts().contains(CodeEditor::tr("Go to Definition")));
    }

    // --- completion popup: mouse select, type-to-filter ---

    //The visible completion popup of the editor (hidden-but-pending-
    //delete ones from a previous round are excluded), null when none.
    QListWidget* visibleCompletionPopup(CodeEditor* editor) {
        for (QListWidget* list : editor->findChildren<QListWidget*>())
            if (list->isVisible())
                return list;
        return nullptr;
    }

    void testCompletionPopupMouseSelect() {
        writeIoPackage();
        langservice::SymbolIndex index;
        index.LoadFile(abs("io.n").toStdString());

        CodeEditor editor;
        editor.setSymbolIndex(&index);
        editor.resize(400, 300);
        editor.show();
        QTest::qWaitForWindowExposed(&editor);

        // "io" + '.' opens the package completion popup.
        editor.setPlainText("io");
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
        QTest::keyClick(&editor, Qt::Key_Period);
        QListWidget* popup = visibleCompletionPopup(&editor);
        QVERIFY2(popup != nullptr, "'.' after a known package opens the popup");
        QCOMPARE(popup->count(), 3);

        //Two invariants make the list clickable at all: it lives inside
        //the editor as a child of the viewport (a top-level window such
        //as Qt::ToolTip would swallow the press) and it never takes the
        //keyboard focus (typing must keep filtering, not go to the list).
        QVERIFY2(!popup->isWindow(), "the popup must not be a top-level");
        QCOMPARE(popup->parentWidget(), editor.viewport());
        QCOMPARE(popup->focusPolicy(), Qt::NoFocus);

        //Clicking the first candidate inserts it after "io." and closes
        //the popup like a menu (visualItemRect is in the list's viewport
        //coordinates, so the click goes to that viewport).
        const QRect itemRect = popup->visualItemRect(popup->item(0));
        QTest::mouseClick(popup->viewport(), Qt::LeftButton, Qt::NoModifier,
                          itemRect.center());
        QVERIFY2(editor.toPlainText().contains(QStringLiteral("io.print")),
                 "clicking a candidate must insert its name");
        QVERIFY2(visibleCompletionPopup(&editor) == nullptr,
                 "applying a candidate must close the popup");
    }

    void testCompletionPopupFiltersAsTyped() {
        writeIoPackage();
        langservice::SymbolIndex index;
        index.LoadFile(abs("io.n").toStdString());

        CodeEditor editor;
        editor.setSymbolIndex(&index);
        editor.resize(400, 300);
        editor.show();
        QTest::qWaitForWindowExposed(&editor);

        auto openPopup = [&editor] {
            editor.setPlainText("io");
            QTextCursor cursor = editor.textCursor();
            cursor.movePosition(QTextCursor::End);
            editor.setTextCursor(cursor);
            QTest::keyClick(&editor, Qt::Key_Period);
        };

        //Typing narrows the list live; the typed text stays in the
        //document ("io.p" after 'p', "io.pr" after 'r').
        openPopup();
        QListWidget* popup = visibleCompletionPopup(&editor);
        QVERIFY(popup != nullptr);
        QCOMPARE(popup->count(), 3);
        QTest::keyClick(&editor, Qt::Key_P);
        popup = visibleCompletionPopup(&editor);
        QVERIFY2(popup != nullptr,
                 "typing an identifier character must keep the popup");
        QCOMPARE(popup->count(), 2);  // print, println
        QCOMPARE(editor.toPlainText(), QString("io.p"));
        QTest::keyClick(&editor, Qt::Key_R);
        QCOMPARE(editor.toPlainText(), QString("io.pr"));

        //Backspace widens the list again (the filter shrinks), and
        //another letter narrows it to a single candidate.
        QTest::keyClick(&editor, Qt::Key_Backspace);
        popup = visibleCompletionPopup(&editor);
        QVERIFY(popup != nullptr);
        QCOMPARE(popup->count(), 2);
        QTest::keyClick(&editor, Qt::Key_Backspace);
        popup = visibleCompletionPopup(&editor);
        QVERIFY(popup != nullptr);
        QCOMPARE(popup->count(), 3);  // empty filter: the full list
        QTest::keyClick(&editor, Qt::Key_R);
        popup = visibleCompletionPopup(&editor);
        QVERIFY(popup != nullptr);
        QCOMPARE(popup->count(), 1);  // readInt

        //Enter applies the current candidate, replacing the typed filter.
        QTest::keyClick(&editor, Qt::Key_Return);
        QVERIFY(editor.toPlainText().contains(QStringLiteral("io.readInt")));
        QVERIFY(visibleCompletionPopup(&editor) == nullptr);

        //A non-identifier key (space) dismisses the popup and reaches
        //the document normally.
        openPopup();
        QVERIFY(visibleCompletionPopup(&editor) != nullptr);
        QTest::keyClick(&editor, Qt::Key_Space);
        QVERIFY2(visibleCompletionPopup(&editor) == nullptr,
                 "a non-identifier key must dismiss the popup");
        QCOMPARE(editor.toPlainText(), QString("io. "));
    }

    // --- hover text formatting ---

    void testFormatSymbol() {
        langservice::SymbolInfo symbol;
        symbol.native = true;
        symbol.pkg = "io";
        symbol.name = "print";
        symbol.returnType = "void";
        symbol.params.push_back({"string", "s"});
        symbol.doc.push_back("Print a value.");

        const QString text = CodeEditor::formatSymbol(symbol);
        QVERIFY(text.contains(QStringLiteral("[native]")));
        QVERIFY(text.contains(QStringLiteral("void io.print(string s)")));
        QVERIFY(text.contains(QStringLiteral("Print a value.")));

        // A nlang-implemented symbol must not carry [native].
        langservice::SymbolInfo plain;
        plain.native = false;
        plain.pkg = "mylib";
        plain.name = "add";
        plain.returnType = "int";
        plain.params.push_back({"int", "a"});
        plain.params.push_back({"int", "b"});
        const QString text2 = CodeEditor::formatSymbol(plain);
        QVERIFY(!text2.contains(QStringLiteral("[native]")));
        QVERIFY(text2.contains(QStringLiteral("int mylib.add(int a, int b)")));
    }

    // --- BreakpointStore: rename + cross-restart load ---

    void testBreakpointStoreRenameMovesLines() {
        BreakpointStore store;
        const QString before = abs("old.n");
        const QString after = abs("new.n");
        QVERIFY(store.toggle(before, 3));
        QVERIFY(store.toggle(before, 7));

        store.rename(before, after);
        QVERIFY(!store.contains(before, 3));
        QVERIFY(!store.contains(before, 7));
        QCOMPARE(store.linesOf(after), QSet<int>({3, 7}));
        QCOMPARE(store.files(),
                 QStringList({BreakpointStore::normalizedKey(after)}));

        //A rename of an unlisted file is a no-op (a rename is not an
        //open, mirroring the recent list).
        store.rename(abs("stranger.n"), abs("stranger2.n"));
        QCOMPARE(store.files().size(), 1);
    }

    void testBreakpointStoreLoadRestoresAfterRestart() {
        //Write through one store, then load into a FRESH one from the
        //same settings scope: the cross-restart path (the ctor's load).
        {
            BreakpointStore store;
            QVERIFY(store.toggle(abs("keep.n"), 4));
            QVERIFY(store.toggle(abs("keep.n"), 9));
            QSettings settings(iniPath(), QSettings::IniFormat);
            store.save(settings);
        }
        BreakpointStore restored;
        QSettings settings(iniPath(), QSettings::IniFormat);
        restored.load(settings);
        QCOMPARE(restored.linesOf(abs("keep.n")), QSet<int>({4, 9}));
    }

    void testBreakpointStoreLoadSkipsCorruptEntries() {
        //load() is the store's only real parsing: one good entry plus
        //the broken shapes (no tab, no line half, non-numeric / <=0
        //lines) -- only the valid data survives.
        QSettings settings(iniPath(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("breakpoints/entries"),
                          QStringList({abs("good.n") + QLatin1Char('\t')
                                           + QLatin1String("2,5"),
                                       abs("notab.n"),
                                       abs("nolines.n") + QLatin1Char('\t'),
                                       abs("badlines.n") + QLatin1Char('\t')
                                           + QLatin1String("x,0,-3")}));
        BreakpointStore store;
        store.load(settings);
        QCOMPARE(store.files(),
                 QStringList({BreakpointStore::normalizedKey(abs("good.n"))}));
        QCOMPARE(store.linesOf(abs("good.n")), QSet<int>({2, 5}));
    }
};

QTEST_MAIN(TestEditor)
#include "test_editor.moc"
