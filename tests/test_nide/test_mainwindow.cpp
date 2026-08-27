/*--- test_mainwindow.cpp - MainWindow integration tests ---*/
#include "MainWindow.h"
#include "CodeEditor.h"
#include "CompileLogBrowser.h"
#include "FileEditor.h"
#include "HelpWindow.h"
#include "ProjectModel.h"
#include "TranslationLoader.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QSplitter>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextLayout>
#include <QTimer>
#include <QTreeView>
#include <QTranslator>
#include <QtTest>

using namespace nlang;

namespace {

//A tiny program whose process exit code nvm propagates (must stay <= 255
//on Windows).
const char* const kMainSource =
    "public int main() {\n"
    "    return 42;\n"
    "}\n";

//Schedule an action to run inside the modal exec() loop that the NEXT
//blocking call opens (queue right before triggering it).
template<typename Fn>
void inExec(Fn action) {
    QTimer::singleShot(0, action);
}

//Chain steps across CONSECUTIVE modal loops: each step is queued from
//inside the previous one, so it fires in the NEXT exec() loop (queueing
//both up front would collapse them into the first loop -- see the
//detailed comment in test_dialogs.cpp).
template<typename Fn1, typename Fn2>
void inExecSteps2(Fn1 step1, Fn2 step2) {
    QTimer::singleShot(0, [step1, step2]() mutable {
        step1();
        QTimer::singleShot(0, step2);
    });
}

//Close the dialog as accepted. QDialog::accept is public but
//QFileDialog re-declares it protected, so go through the meta-object
//(name-based invocation ignores C++ access).
void acceptDialog(QDialog* dialog) {
    QMetaObject::invokeMethod(dialog, "accept");
}

//Answer the active QMessageBox with the given button; a no-op when the
//active modal is not a message box.
void answerMessageBox(QMessageBox::StandardButton button) {
    if (QMessageBox* box =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
        box->button(button)->click();
}

//Accept the active (non-native) file dialog with the chosen path.
void acceptFileDialog(const QString& filePath) {
    if (QFileDialog* dialog =
            qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
        dialog->selectFile(filePath);
        acceptDialog(dialog);
    }
}

//Accept the active project-properties dialog with name/dir/namespace.
void acceptProjectDialog(const QString& name, const QString& dir) {
    QDialog* dialog =
        qobject_cast<QDialog*>(QApplication::activeModalWidget());
    QLineEdit* nameEdit =
        dialog != nullptr ? dialog->findChild<QLineEdit*>("edtProjectName")
                          : nullptr;
    if (nameEdit != nullptr) {
        nameEdit->setText(name);
        dialog->findChild<QLineEdit*>("edtProjectDir")->setText(dir);
        dialog->findChild<QLineEdit*>("edtNamespace")->setText("app");
        acceptDialog(dialog);
    }
}

//Accept the active new-file dialog with the given file name (the
//directory field keeps its per-project default; an explicit directory
//overrides it -- the no-solution case would default to the CWD).
void acceptNewFileDialog(const QString& fileName,
                         const QString& directory = QString()) {
    QDialog* dialog =
        qobject_cast<QDialog*>(QApplication::activeModalWidget());
    QLineEdit* nameEdit =
        dialog != nullptr ? dialog->findChild<QLineEdit*>("edtName")
                          : nullptr;
    if (nameEdit != nullptr) {
        nameEdit->setText(fileName);
        if (!directory.isEmpty())
            dialog->findChild<QLineEdit*>("edtDirectory")
                ->setText(directory);
        acceptDialog(dialog);
    }
}

QAction* act(MainWindow& window, const char* name) {
    return window.findChild<QAction*>(name);
}

QTabWidget* tabCodes(MainWindow& window) {
    return window.findChild<QTabWidget*>("tabCodes");
}

QTreeView* solutionView(MainWindow& window) {
    return window.findChild<QTreeView*>("tvwSolution");
}

CodeEditor* currentCode(MainWindow& window) {
    return qobject_cast<CodeEditor*>(tabCodes(window)->currentWidget());
}

//The first file row (solution -> project -> file); the single-project
//fixtures all have exactly one.
QModelIndex firstFileIndex(MainWindow& window) {
    QAbstractItemModel* model = solutionView(window)->model();
    return model->index(0, 0, model->index(0, 0, model->index(0, 0)));
}

//A source file on disk.
void writeFile(const QString& filePath, const char* content) {
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(content);
}

//Foreground color the highlighter applied at a block position
//(mirrors the helper in test_syntaxhighlighter.cpp); invalid color
//when the position carries no format span.
QColor colorAt(const QTextBlock& block, int pos) {
    for (const QTextLayout::FormatRange& range : block.layout()->formats()) {
        if (pos >= range.start && pos < range.start + range.length)
            return range.format.foreground().color();
    }
    return QColor();
}

//An on-disk project fixture: <dir>/App/{App.nproj,main.n}. Returns the
//two paths through the out-params.
void writeProjectFixture(const QString& baseDir, QString* nprojPath,
                         QString* mainPath) {
    QDir(baseDir).mkpath("App");
    *nprojPath = QDir(baseDir).filePath("App/App.nproj");
    *mainPath = QDir(baseDir).filePath("App/main.n");
    writeFile(*nprojPath,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Project name=\"App\">\n"
        "  <Sources>\n"
        "    <File path=\"main.n\"/>\n"
        "  </Sources>\n"
        "</Project>\n");
    writeFile(*mainPath, kMainSource);
}

//Open the fixture project through the real menu action + file dialog
//(ensureSolution, load, select). Returns the project's tree index.
QModelIndex openFixtureProject(MainWindow& window, const QString& baseDir) {
    QString nprojPath;
    QString mainPath;
    writeProjectFixture(baseDir, &nprojPath, &mainPath);
    inExec([&] { acceptFileDialog(nprojPath); });
    act(window, "actOpenProject")->trigger();

    const QModelIndex solutionIndex = solutionView(window)->model()->index(0, 0);
    return solutionView(window)->model()->index(0, 0, solutionIndex);
}

//Commit an inline rename on the first file row through the view's real
//editor (the same path F2 takes): edit() -> delegate -> commitData ->
//model setData -> fileRenameRequested. commitData is protected, so the
//call goes through the meta-object (name-based, like acceptDialog).
void renameViaTree(MainWindow& window, const QString& newName) {
    QTreeView* view = solutionView(window);
    const QModelIndex fileIndex = firstFileIndex(window);
    view->setCurrentIndex(fileIndex);
    view->edit(fileIndex);  // Qt5's edit(index) returns void
    QLineEdit* nameEdit = qobject_cast<QLineEdit*>(view->focusWidget());
    QVERIFY(nameEdit != nullptr);
    nameEdit->setText(newName);
    QMetaObject::invokeMethod(view, "commitData",
                              Q_ARG(QWidget*, nameEdit));
}

//The text inside a file on disk (empty on open failure).
QString readTextFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

//An exec()ed QMenu is a POPUP, not a modal widget; when driven
//synthetic (no real mouse), it may register as neither active popup
//nor active modal -- fall back to a scan for a visible top-level menu.
QMenu* activeMenu() {
    if (QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        return menu;
    if (QMenu* menu = qobject_cast<QMenu*>(QApplication::activeModalWidget()))
        return menu;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (QMenu* menu = qobject_cast<QMenu*>(widget)) {
            if (menu->isVisible())
                return menu;
        }
    }
    return nullptr;
}

//Click the named action on the active popup menu the way a user does:
//QMenu::exec only reports actions chosen through the menu's own
//activation, so a raw action->trigger() never reaches the exec return.
void clickMenuAction(const QString& text) {
    QMenu* menu = activeMenu();
    if (menu == nullptr)
        return;
    for (QAction* action : menu->actions()) {
        if (action->text() == text && action->isEnabled()) {
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                              menu->actionGeometry(action).center());
            return;
        }
    }
}

//Close the active popup menu without choosing anything (a check-only
//visit still leaves the exec() loop).
void closeActiveMenu() {
    if (QMenu* menu = activeMenu())
        menu->close();
}

//Accept the active input dialog with the given text.
void acceptInputDialog(const QString& text) {
    if (QInputDialog* dialog =
            qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
        dialog->setTextValue(text);
        acceptDialog(dialog);
    }
}

//Menu handlers open their follow-up modal SYNCHRONOUSLY inside the
//trigger call, so the answer cannot be queued from after the menu step
//(it would deadlock behind the nested loop). Retry instead: the helper
//re-fires until the expected modal shows up, then answers it.
void acceptInputDialogSoon(const QString& text, int retries = 20) {
    //Zero-delay retries would burn out inside the menu's loop before
    //the nested dialog appears; 20ms spans the transition.
    QTimer::singleShot(20, [text, retries]() {
        if (qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            acceptInputDialog(text);
        } else if (retries > 0) {
            acceptInputDialogSoon(text, retries - 1);
        }
    });
}

//Same retry pattern for a message box (see acceptInputDialogSoon).
void answerMessageBoxSoon(QMessageBox::StandardButton button,
                          int retries = 20) {
    QTimer::singleShot(20, [button, retries]() {
        QMessageBox* box =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box != nullptr && box->button(button) != nullptr)
            box->button(button)->click();
        else if (retries > 0)
            answerMessageBoxSoon(button, retries - 1);
    });
}

} // namespace

class TestMainWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        //Static QFileDialog::get* calls must produce drivable QFileDialog
        //widgets, not native platform dialogs.
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        qRegisterMetaType<nlang::CompileLogItemInfo>(
            "nlang::CompileLogItemInfo");
        //The MainWindow ctor restores layout from DEFAULT-constructed
        //QSettings: pin the app name so tests never touch the real
        //nide registry entry.
        QCoreApplication::setOrganizationName(QStringLiteral("NLang"));
        QCoreApplication::setApplicationName(QStringLiteral("nide-test"));
    }

    //--- initial state ---

    void testInitialMenuState() {
        MainWindow window;

        QVERIFY(!act(window, "actSaveFile")->isEnabled());
        QVERIFY(!act(window, "actSaveFileAs")->isEnabled());
        QVERIFY(!act(window, "actCloseFile")->isEnabled());
        QVERIFY(!act(window, "actSaveProject")->isEnabled());
        QVERIFY(!act(window, "actCloseProject")->isEnabled());
        QVERIFY(!act(window, "actAddExistFile")->isEnabled());
        QVERIFY(!act(window, "actAddNewFile")->isEnabled());
        QVERIFY(!act(window, "actRemoveFile")->isEnabled());
        QVERIFY(!act(window, "actProjectProp")->isEnabled());
        QVERIFY(!act(window, "actBuild")->isEnabled());
        QVERIFY(!act(window, "actSaveSolution")->isEnabled());
        QVERIFY(!act(window, "actCloseSolution")->isEnabled());
        QVERIFY(act(window, "actNewSolution")->isEnabled());
        QVERIFY(act(window, "actOpenSolution")->isEnabled());
        //Start needs a selected project AND an idle run; Stop only a
        //running process. Neither holds initially.
        QVERIFY(!act(window, "actStartRunning")->isEnabled());
        QVERIFY(!act(window, "actStopRunning")->isEnabled());
        QCOMPARE(tabCodes(window)->count(), 0);
        QCOMPARE(solutionView(window)->model()->rowCount(), 0);
    }

    //--- solution lifecycle ---

    void testNewSolution() {
        MainWindow window;
        act(window, "actNewSolution")->trigger();

        QAbstractItemModel* model = solutionView(window)->model();
        QCOMPARE(model->rowCount(), 1);
        const QModelIndex solutionIndex = model->index(0, 0);
        QCOMPARE(solutionIndex.data().toString(), QString("Solution1"));
        QCOMPARE(model->rowCount(solutionIndex), 0);  // no projects yet
        QVERIFY(act(window, "actSaveSolution")->isEnabled());
        QVERIFY(act(window, "actCloseSolution")->isEnabled());
        //Nothing is selected: project-scoped actions stay off.
        QVERIFY(!act(window, "actBuild")->isEnabled());
    }

    void testCloseSolutionDiscardDirty() {
        MainWindow window;
        act(window, "actNewSolution")->trigger();

        //Adding a project marks the solution dirty (solution-level
        //state, not per-project isDirty).
        QTemporaryDir dir;
        inExec([&] { acceptProjectDialog("App", dir.path()); });
        act(window, "actNewProject")->trigger();
        QVERIFY(act(window, "actBuild")->isEnabled());  // project selected

        inExec([&] { answerMessageBox(QMessageBox::Discard); });
        act(window, "actCloseSolution")->trigger();

        QCOMPARE(solutionView(window)->model()->rowCount(), 0);
        QVERIFY(!act(window, "actSaveSolution")->isEnabled());
        QVERIFY(!act(window, "actBuild")->isEnabled());
    }

    void testOpenSolution() {
        MainWindow window;
        QTemporaryDir dir;
        QString nprojPath;
        QString mainPath;
        writeProjectFixture(dir.path(), &nprojPath, &mainPath);
        const QString slnPath = QDir(dir.path()).filePath("IdeSln.nsln");
        writeFile(slnPath,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Solution name=\"Sln\">\n"
            "  <Projects>\n"
            "    <Project path=\"App/App.nproj\"/>\n"
            "  </Projects>\n"
            "</Solution>\n");

        inExec([&] { acceptFileDialog(slnPath); });
        act(window, "actOpenSolution")->trigger();

        QAbstractItemModel* model = solutionView(window)->model();
        QCOMPARE(model->rowCount(), 1);
        const QModelIndex solutionIndex = model->index(0, 0);
        QCOMPARE(solutionIndex.data().toString(), QString("Sln"));
        //Deep load: the project and its file arrived too.
        const QModelIndex projectIndex = model->index(0, 0, solutionIndex);
        QCOMPARE(projectIndex.data().toString(), QString("App"));
        QCOMPARE(model->rowCount(projectIndex), 1);
        //Loading selects nothing: project-scoped actions stay off.
        QVERIFY(!act(window, "actBuild")->isEnabled());
    }

    //--- projects ---

    void testNewProjectViaDialog() {
        MainWindow window;
        act(window, "actNewSolution")->trigger();

        QTemporaryDir dir;
        inExec([&] { acceptProjectDialog("App", dir.path()); });
        act(window, "actNewProject")->trigger();

        QAbstractItemModel* model = solutionView(window)->model();
        const QModelIndex solutionIndex = model->index(0, 0);
        QCOMPARE(model->rowCount(solutionIndex), 1);
        QCOMPARE(model->index(0, 0, solutionIndex).data().toString(),
                 QString("App"));
        //selectProject picked the new row: project actions are live.
        QVERIFY(act(window, "actBuild")->isEnabled());
        QVERIFY(act(window, "actSaveProject")->isEnabled());
        QVERIFY(act(window, "actAddNewFile")->isEnabled());
        //Creation only registers the project; the .nproj is not on disk.
        QVERIFY(!QFileInfo::exists(QDir(dir.path()).filePath("App.nproj")));
    }

    void testOpenProjectAndDoubleClickedFile() {
        MainWindow window;
        QTemporaryDir dir;
        const QModelIndex projectIndex =
            openFixtureProject(window, dir.path());

        QAbstractItemModel* model = solutionView(window)->model();
        QCOMPARE(projectIndex.data().toString(), QString("App"));
        QCOMPARE(model->rowCount(projectIndex), 1);
        const QModelIndex fileIndex = model->index(0, 0, projectIndex);
        QCOMPARE(fileIndex.data().toString(), QString("main.n"));
        QVERIFY(act(window, "actBuild")->isEnabled());
        //The project row (not a file) is selected after opening: file
        //removal stays unavailable until a file row is selected.
        QVERIFY(!act(window, "actRemoveFile")->isEnabled());

        //Double-clicking the file opens an editor with the disk content.
        QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
                                  Q_ARG(QModelIndex, fileIndex));
        QCOMPARE(tabCodes(window)->count(), 1);
        QCOMPARE(tabCodes(window)->tabText(0), QString("main.n"));
        QVERIFY(currentCode(window)->toPlainText().contains("return 42"));
        QVERIFY(act(window, "actSaveFile")->isEnabled());
    }

    void testRemoveFileFromProject() {
        MainWindow window;
        QTemporaryDir dir;
        const QModelIndex projectIndex =
            openFixtureProject(window, dir.path());

        const QModelIndex fileIndex =
            solutionView(window)->model()->index(0, 0, projectIndex);
        solutionView(window)->setCurrentIndex(fileIndex);
        QVERIFY(act(window, "actRemoveFile")->isEnabled());

        act(window, "actRemoveFile")->trigger();
        QCOMPARE(solutionView(window)->model()->rowCount(projectIndex), 0);
        QVERIFY(!act(window, "actRemoveFile")->isEnabled());
    }

    void testAddExistingFile() {
        MainWindow window;
        QTemporaryDir dir;
        const QModelIndex projectIndex =
            openFixtureProject(window, dir.path());

        const QString extraPath = QDir(dir.path()).filePath("App/util.n");
        writeFile(extraPath, "int util() { return 1; }\n");
        inExec([&] { acceptFileDialog(extraPath); });
        act(window, "actAddExistFile")->trigger();

        QCOMPARE(solutionView(window)->model()->rowCount(projectIndex), 2);
    }

    void testCloseProject() {
        //A loaded (clean) project closes silently -- no modal, nothing
        //to answer.
        {
            MainWindow window;
            QTemporaryDir dir;
            openFixtureProject(window, dir.path());
            QVERIFY(act(window, "actCloseProject")->isEnabled());

            act(window, "actCloseProject")->trigger();

            QAbstractItemModel* model = solutionView(window)->model();
            const QModelIndex solutionIndex = model->index(0, 0);
            QCOMPARE(model->rowCount(solutionIndex), 0);
            QVERIFY(!act(window, "actBuild")->isEnabled());
        }

        //A project with an added-but-unsaved file prompts once; Discard
        //removes it anyway.
        {
            MainWindow window;
            QTemporaryDir dir;
            openFixtureProject(window, dir.path());
            const QString extraPath =
                QDir(dir.path()).filePath("App/util.n");
            writeFile(extraPath, "int util() { return 1; }\n");
            inExec([&] { acceptFileDialog(extraPath); });
            act(window, "actAddExistFile")->trigger();

            inExec([&] { answerMessageBox(QMessageBox::Discard); });
            act(window, "actCloseProject")->trigger();

            QVERIFY(!act(window, "actBuild")->isEnabled());
        }
    }

    //--- editors ---

    void testEditorDirtySaveClose() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        const QString mainPath = QDir(dir.path()).filePath("App/main.n");
        QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
            Q_ARG(QModelIndex, firstFileIndex(window)));

        //An edit dirties the editor: the tab title gains a "*".
        currentCode(window)->appendPlainText("    int x = 1;");
        QCOMPARE(tabCodes(window)->tabText(0), QString("main.n*"));

        act(window, "actSaveFile")->trigger();
        QCOMPARE(tabCodes(window)->tabText(0), QString("main.n"));
        QFile onDisk(mainPath);
        QVERIFY(onDisk.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(onDisk.readAll()).contains("int x = 1;"));

        //A clean close prompts nothing.
        act(window, "actCloseFile")->trigger();
        QCOMPARE(tabCodes(window)->count(), 0);
    }

    void testSaveFileAs() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
            Q_ARG(QModelIndex, firstFileIndex(window)));

        const QString copyPath = QDir(dir.path()).filePath("App/copy.n");
        inExec([&] { acceptFileDialog(copyPath); });
        act(window, "actSaveFileAs")->trigger();

        QCOMPARE(tabCodes(window)->tabText(0), QString("copy.n"));
        QVERIFY(QFileInfo::exists(copyPath));
    }

    void testNewFileAddedToProject() {
        MainWindow window;
        QTemporaryDir dir;
        const QModelIndex projectIndex =
            openFixtureProject(window, dir.path());

        inExec([&] { acceptNewFileDialog("created.n"); });
        act(window, "actAddNewFile")->trigger();

        //The file exists on disk, joined the project, and is being edited.
        QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath("App/created.n")));
        QCOMPARE(solutionView(window)->model()->rowCount(projectIndex), 2);
        QCOMPARE(tabCodes(window)->count(), 1);
        QCOMPARE(tabCodes(window)->tabText(0), QString("created.n"));
    }

    void testNewFileFromMenuJoinsSelectedProject() {
        MainWindow window;
        QTemporaryDir dir;
        const QModelIndex projectIndex =
            openFixtureProject(window, dir.path());

        inExec([&] { acceptNewFileDialog("extra.n"); });
        act(window, "actNewFile")->trigger();

        //File->New joins the selected project, not just the editor tab.
        QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath("App/extra.n")));
        QCOMPARE(solutionView(window)->model()->rowCount(projectIndex), 2);
        QCOMPARE(solutionView(window)->currentIndex().data().toString(),
                 QString("extra.n"));
        QCOMPARE(tabCodes(window)->count(), 1);
        QCOMPARE(tabCodes(window)->tabText(0), QString("extra.n"));
    }

    void testNewFileFromMenuJoinsSoleProjectWithoutSelection() {
        MainWindow window;
        QTemporaryDir dir;
        const QModelIndex projectIndex =
            openFixtureProject(window, dir.path());

        //Selection moved to the solution root: no project row is
        //selected, so the sole project takes the file.
        solutionView(window)->setCurrentIndex(
            solutionView(window)->model()->index(0, 0));
        inExec([&] { acceptNewFileDialog("extra.n"); });
        act(window, "actNewFile")->trigger();

        QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath("App/extra.n")));
        QCOMPARE(solutionView(window)->model()->rowCount(projectIndex), 2);
        QCOMPARE(tabCodes(window)->count(), 1);
    }

    void testNewFileWithoutSolutionOpensStandaloneEditor() {
        MainWindow window;
        QTemporaryDir dir;

        //No solution open: the new file stays standalone, mirrored in
        //the tree's standalone group.
        inExec([&] { acceptNewFileDialog("extra.n", dir.path()); });
        act(window, "actNewFile")->trigger();

        QAbstractItemModel* model = solutionView(window)->model();
        QCOMPARE(model->rowCount(), 1);  // the standalone group
        QCOMPARE(model->index(0, 0).data().toString(),
                 QString("Standalone Files"));
        QCOMPARE(model->index(0, 0, model->index(0, 0)).data().toString(),
                 QString("extra.n"));
        QCOMPARE(tabCodes(window)->count(), 1);
        QCOMPARE(tabCodes(window)->tabText(0), QString("extra.n"));
        QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath("extra.n")));
    }

    //--- file rename (tree inline edit pipeline) ---

    void testRenameFileWithDirtyEditorSavesAndFollows() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
            Q_ARG(QModelIndex, firstFileIndex(window)));
        const QString oldPath = QDir(dir.path()).filePath("App/main.n");
        const QString newPath = QDir(dir.path()).filePath("App/renamed.n");

        currentCode(window)->appendPlainText("// dirty\n");
        renameViaTree(window, "renamed.n");

        //Save-then-rename: the dirty content reached the new name and
        //the old one is gone (a move, not a copy).
        QVERIFY(!QFileInfo::exists(oldPath));
        QVERIFY(QFileInfo::exists(newPath));
        QVERIFY(readTextFile(newPath).contains("// dirty"));
        //Tree and tab follow the new name; the editor is clean after
        //the implicit save (no asterisk).
        QCOMPARE(firstFileIndex(window).data().toString(),
                 QString("renamed.n"));
        QCOMPARE(tabCodes(window)->tabText(0), QString("renamed.n"));
        //A later edit saves through the NEW path.
        currentCode(window)->appendPlainText("// more\n");
        act(window, "actSaveFile")->trigger();
        QVERIFY(readTextFile(newPath).contains("// more"));
    }

    void testRenameFileWithoutEditorOpen() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        const QString oldPath = QDir(dir.path()).filePath("App/main.n");
        const QString newPath = QDir(dir.path()).filePath("App/renamed.n");

        renameViaTree(window, "renamed.n");

        QVERIFY(!QFileInfo::exists(oldPath));
        QVERIFY(readTextFile(newPath).contains("return 42"));
        QCOMPARE(firstFileIndex(window).data().toString(),
                 QString("renamed.n"));
        QCOMPARE(tabCodes(window)->count(), 0);
    }

    void testRenameFileInvalidNameRejected() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        const QString oldPath = QDir(dir.path()).filePath("App/main.n");

        inExec([&] { answerMessageBox(QMessageBox::Ok); });  // warning
        renameViaTree(window, "bad/name.n");

        //Nothing moved: disk, tree and editors all stay on the old name.
        QVERIFY(QFileInfo::exists(oldPath));
        QCOMPARE(firstFileIndex(window).data().toString(),
                 QString("main.n"));
        QCOMPARE(tabCodes(window)->count(), 0);
    }

    void testRenameFileLockedOnDiskKeepsEverything() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
            Q_ARG(QModelIndex, firstFileIndex(window)));
        const QString oldPath = QDir(dir.path()).filePath("App/main.n");
        const QString newPath = QDir(dir.path()).filePath("App/renamed.n");

        //An external read handle blocks both the disk rename AND any
        //write through the old path (verified the hard way: Windows
        //denies the write too), so the save check runs after the lock
        //is released -- success paths open no dialogs.
        QFile lock(oldPath);
        QVERIFY(lock.open(QIODevice::ReadOnly));
        inExec([&] { answerMessageBox(QMessageBox::Ok); });  // warning
        renameViaTree(window, "renamed.n");

        QVERIFY(QFileInfo::exists(oldPath));
        QVERIFY(!QFileInfo::exists(newPath));
        QCOMPARE(firstFileIndex(window).data().toString(),
                 QString("main.n"));
        //The editor kept the old path: once the lock is gone, a later
        //edit saves through it (nothing was redirected to the new name).
        lock.close();
        currentCode(window)->appendPlainText("// still here\n");
        act(window, "actSaveFile")->trigger();
        QVERIFY(readTextFile(oldPath).contains("// still here"));
    }

    void testRenameCaseVariantChangesCasingEverywhere() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
            Q_ARG(QModelIndex, firstFileIndex(window)));
        currentCode(window)->appendPlainText("// dirty\n");

        renameViaTree(window, "Main.n");

        //exists() folds case on Windows, so read the directory's actual
        //entry names: the casing must have moved.
        QVERIFY(QDir(QDir(dir.path()).filePath("App"))
                    .entryList(QStringList() << "*.n", QDir::Files)
                    .contains("Main.n"));
        QCOMPARE(firstFileIndex(window).data().toString(),
                 QString("Main.n"));
        //The dirty save ran before the move, so the tab is clean.
        QCOMPARE(tabCodes(window)->tabText(0), QString("Main.n"));
        QVERIFY(readTextFile(QDir(dir.path()).filePath("App/Main.n"))
                    .contains("// dirty"));
    }

    void testRenameDomainRejectionRollsDiskBack() {
        MainWindow window;
        QTemporaryDir dir;
        QString nprojPath;
        QString mainPath;
        writeProjectFixture(dir.path(), &nprojPath, &mainPath);
        //A stale project entry: ghost.n is listed in the .nproj but
        //absent on disk, so the disk-exists check passes and only the
        //domain's dedup catches the clash.
        writeFile(nprojPath,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project name=\"App\">\n"
            "  <Sources>\n"
            "    <File path=\"main.n\"/>\n"
            "    <File path=\"ghost.n\"/>\n"
            "  </Sources>\n"
            "</Project>\n");
        inExec([&] { acceptFileDialog(nprojPath); });
        act(window, "actOpenProject")->trigger();

        const QString ghostPath = QDir(dir.path()).filePath("App/ghost.n");
        inExec([&] { answerMessageBox(QMessageBox::Ok); });  // error
        renameViaTree(window, "ghost.n");

        //The rollback restored the pre-rename disk state.
        QVERIFY(QFileInfo::exists(mainPath));
        QVERIFY(!QFileInfo::exists(ghostPath));
        QCOMPARE(firstFileIndex(window).data().toString(),
                 QString("main.n"));
    }

    //--- context menus (tree + tab bar) ---

    void testTreeContextMenuRenameEditsInPlace() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        const QString newPath = QDir(dir.path()).filePath("App/renamed.n");

        QTreeView* view = solutionView(window);
        const QModelIndex fileIndex = firstFileIndex(window);
        const QPoint pos = view->visualRect(fileIndex).center();
        inExec([&] { clickMenuAction(MainWindow::tr("Rename (F2)")); });
        QMetaObject::invokeMethod(view, "customContextMenuRequested",
                                  Q_ARG(QPoint, pos));

        //The handler opened the inline editor once the menu closed.
        QLineEdit* nameEdit = view->findChild<QLineEdit*>();
        QVERIFY2(nameEdit != nullptr, "inline editor must be open");
        nameEdit->setText("renamed.n");
        QMetaObject::invokeMethod(view, "commitData",
                                  Q_ARG(QWidget*, nameEdit));

        QVERIFY(QFileInfo::exists(newPath));
        QCOMPARE(firstFileIndex(window).data().toString(),
                 QString("renamed.n"));
    }

    void testTreeContextMenuRenameDisabledOnProjectRow() {
        MainWindow window;
        QTemporaryDir dir;
        const QModelIndex projectIndex =
            openFixtureProject(window, dir.path());
        QTreeView* view = solutionView(window);
        const QPoint pos = view->visualRect(projectIndex).center();

        inExec([&] {
            QMenu* menu = activeMenu();
            QVERIFY2(menu != nullptr, "context menu must open");
            for (QAction* action : menu->actions()) {
                if (action->text() == MainWindow::tr("Rename (F2)"))
                    QVERIFY(!action->isEnabled());
            }
            menu->close();
        });
        QMetaObject::invokeMethod(view, "customContextMenuRequested",
                                  Q_ARG(QPoint, pos));
    }

    void testTabContextMenuRenameViaInputDialog() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
            Q_ARG(QModelIndex, firstFileIndex(window)));
        const QString newPath = QDir(dir.path()).filePath("App/renamed.n");

        //The menu handler opens the input dialog synchronously inside
        //its trigger, so the answer is scheduled as a retry (see
        //acceptInputDialogSoon).
        QTabBar* bar = tabCodes(window)->tabBar();
        const QPoint pos = bar->tabRect(0).center();
        acceptInputDialogSoon("renamed.n");
        inExec([&] { clickMenuAction(MainWindow::tr("Rename...")); });
        QMetaObject::invokeMethod(bar, "customContextMenuRequested",
                                  Q_ARG(QPoint, pos));

        QVERIFY(QFileInfo::exists(newPath));
        QCOMPARE(tabCodes(window)->tabText(0), QString("renamed.n"));
    }

    //A file opened outside any project: the tab menu is its only rename
    //entry, and the pipeline runs with trackedFile == null (no domain,
    //no tree selection -- just disk + editor).
    void testTabContextMenuRenameStandaloneFile() {
        MainWindow window;
        QTemporaryDir dir;
        const QString oldPath = QDir(dir.path()).filePath("solo.n");
        writeFile(oldPath, kMainSource);
        inExec([&] { acceptFileDialog(oldPath); });
        act(window, "actOpenFile")->trigger();
        const QString newPath = QDir(dir.path()).filePath("solo2.n");

        //Before the rename: the tree's group row mirrors the old name.
        QAbstractItemModel* treeModel = solutionView(window)->model();
        QCOMPARE(treeModel->index(0, 0, treeModel->index(0, 0))
                     .data().toString(),
                 QString("solo.n"));

        QTabBar* bar = tabCodes(window)->tabBar();
        const QPoint pos = bar->tabRect(0).center();
        acceptInputDialogSoon("solo2.n");
        inExec([&] { clickMenuAction(MainWindow::tr("Rename...")); });
        QMetaObject::invokeMethod(bar, "customContextMenuRequested",
                                  Q_ARG(QPoint, pos));

        QVERIFY(!QFileInfo::exists(oldPath));
        QVERIFY(readTextFile(newPath).contains("return 42"));
        QCOMPARE(tabCodes(window)->tabText(0), QString("solo2.n"));
        //The group row followed the rename for free
        //(onEditorSaveStateChanged ends in updateMenuState).
        QCOMPARE(treeModel->index(0, 0).data().toString(),
                 QString("Standalone Files"));
        QCOMPARE(treeModel->index(0, 0, treeModel->index(0, 0))
                     .data().toString(),
                 QString("solo2.n"));
    }

    void testTabContextMenuSaveNonCurrentTab() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        const QString mainPath = QDir(dir.path()).filePath("App/main.n");
        QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
            Q_ARG(QModelIndex, firstFileIndex(window)));
        const QString secondPath = QDir(dir.path()).filePath("App/second.n");
        writeFile(secondPath, kMainSource);
        inExec([&] { acceptFileDialog(secondPath); });
        act(window, "actOpenFile")->trigger();

        //main.n is dirty AND a background tab.
        tabCodes(window)->setCurrentIndex(0);
        currentCode(window)->appendPlainText("// dirty\n");
        QCOMPARE(tabCodes(window)->tabText(0), QString("main.n*"));
        tabCodes(window)->setCurrentIndex(1);

        QTabBar* bar = tabCodes(window)->tabBar();
        const QPoint pos = bar->tabRect(0).center();
        inExec([&] { clickMenuAction(MainWindow::tr("Save")); });
        QMetaObject::invokeMethod(bar, "customContextMenuRequested",
                                  Q_ARG(QPoint, pos));

        //The save reached the background tab (and it became current,
        //like the menu's other file actions).
        QCOMPARE(tabCodes(window)->tabText(0), QString("main.n"));
        QCOMPARE(tabCodes(window)->currentIndex(), 0);
        QVERIFY(readTextFile(mainPath).contains("// dirty"));
    }

    void testTabContextMenuCloseOthers() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
            Q_ARG(QModelIndex, firstFileIndex(window)));
        const QString secondPath = QDir(dir.path()).filePath("App/second.n");
        writeFile(secondPath, kMainSource);
        inExec([&] { acceptFileDialog(secondPath); });
        act(window, "actOpenFile")->trigger();

        //main.n (tab 0) dirty; keep tab 1.
        tabCodes(window)->setCurrentIndex(0);
        currentCode(window)->appendPlainText("// dirty\n");
        QCOMPARE(tabCodes(window)->count(), 2);

        QTabBar* bar = tabCodes(window)->tabBar();
        const QPoint pos = bar->tabRect(1).center();
        //The dirty tab's prompt opens synchronously inside the menu
        //action, hence the retry-based answer.
        answerMessageBoxSoon(QMessageBox::Discard);
        inExec([&] { clickMenuAction(MainWindow::tr("Close Others")); });
        QMetaObject::invokeMethod(bar, "customContextMenuRequested",
                                  Q_ARG(QPoint, pos));

        QCOMPARE(tabCodes(window)->count(), 1);
        QCOMPARE(tabCodes(window)->tabText(0), QString("second.n"));
    }

    //--- standalone files: tree membership ---

    void testOpenStandaloneFileJoinsTree() {
        MainWindow window;
        QTemporaryDir dir;
        const QString path = QDir(dir.path()).filePath("solo.n");
        writeFile(path, kMainSource);
        inExec([&path] { acceptFileDialog(path); });
        act(window, "actOpenFile")->trigger();

        QAbstractItemModel* model = solutionView(window)->model();
        QCOMPARE(model->rowCount(), 1);  // group, no solution open
        QCOMPARE(model->index(0, 0).data().toString(),
                 QString("Standalone Files"));
        QCOMPARE(model->rowCount(model->index(0, 0)), 1);
        QCOMPARE(model->index(0, 0, model->index(0, 0)).data().toString(),
                 QString("solo.n"));
        QCOMPARE(tabCodes(window)->count(), 1);
    }

    void testCloseStandaloneTabRemovesGroupRow() {
        MainWindow window;
        QTemporaryDir dir;
        const QString path = QDir(dir.path()).filePath("solo.n");
        writeFile(path, kMainSource);
        inExec([&path] { acceptFileDialog(path); });
        act(window, "actOpenFile")->trigger();
        QCOMPARE(solutionView(window)->model()->rowCount(), 1);

        // The tab-bar close path (same route as the close button).
        QMetaObject::invokeMethod(&window, "on_tabCodes_tabCloseRequested",
                                  Q_ARG(int, 0));
        QCOMPARE(solutionView(window)->model()->rowCount(), 0);
        QCOMPARE(tabCodes(window)->count(), 0);
    }

    void testOpenProjectAdoptsStandaloneEditor() {
        MainWindow window;
        QTemporaryDir dir;
        QString nprojPath, mainPath;
        writeProjectFixture(dir.path(), &nprojPath, &mainPath);
        // The project's file opened FIRST: standalone until tracked.
        inExec([&mainPath] { acceptFileDialog(mainPath); });
        act(window, "actOpenFile")->trigger();
        QAbstractItemModel* model = solutionView(window)->model();
        QCOMPARE(model->rowCount(), 1);  // the standalone group

        inExec([&nprojPath] { acceptFileDialog(nprojPath); });
        act(window, "actOpenProject")->trigger();
        // Tracked now: solution -> project -> main.n, no group row.
        QCOMPARE(model->rowCount(), 1);  // the solution root
        const QModelIndex solutionIndex = model->index(0, 0);
        QCOMPARE(model->rowCount(solutionIndex), 1);  // the project
        const QModelIndex projectIndex =
            model->index(0, 0, solutionIndex);
        QCOMPARE(model->rowCount(projectIndex), 1);   // the file
        QCOMPARE(model->index(0, 0, projectIndex).data().toString(),
                 QString("main.n"));

        // A second standalone editor under an OPEN solution: the group
        // sits at the solution root's end, and closing its tab removes
        // the group while the solution stays (open-state empty removal).
        const QString extra = QDir(dir.path()).filePath("extra.n");
        writeFile(extra, kMainSource);
        inExec([&extra] { acceptFileDialog(extra); });
        act(window, "actOpenFile")->trigger();
        QCOMPARE(model->rowCount(model->index(0, 0)), 2);  // project + group
        QMetaObject::invokeMethod(&window, "on_tabCodes_tabCloseRequested",
                                  Q_ARG(int, 1));  // extra's tab (0 = main.n)
        QCOMPARE(model->rowCount(model->index(0, 0)), 1);  // project only

        // Closing the solution flips main.n's editor back to standalone
        // (the reverse membership flip). The implicitly created solution
        // is still dirty (the project was never saved into a solution),
        // so Discard answers the close prompt.
        inExec([&] { answerMessageBox(QMessageBox::Discard); });
        act(window, "actCloseSolution")->trigger();
        QCOMPARE(model->rowCount(), 1);  // the group again
        QCOMPARE(model->index(0, 0).data().toString(),
                 QString("Standalone Files"));
        QCOMPARE(model->index(0, 0, model->index(0, 0)).data().toString(),
                 QString("main.n"));
    }

    //A standalone row only exists while its editor is open (the group
    //mirrors open untracked editors), so "reopen" here is the real
    //user-facing behavior: refocusing a backgrounded standalone tab
    //from the tree opens no second editor.
    void testDoubleClickStandaloneRowReopensTab() {
        MainWindow window;
        QTemporaryDir dir;
        //Names picked so path order (a.n < b.n) matches open order: the
        //group lists editors in EditorManager's path-keyed order.
        const QString firstPath = QDir(dir.path()).filePath("a.n");
        writeFile(firstPath, kMainSource);
        inExec([&firstPath] { acceptFileDialog(firstPath); });
        act(window, "actOpenFile")->trigger();
        const QString secondPath = QDir(dir.path()).filePath("b.n");
        writeFile(secondPath, kMainSource);
        inExec([&secondPath] { acceptFileDialog(secondPath); });
        act(window, "actOpenFile")->trigger();
        QCOMPARE(tabCodes(window)->count(), 2);

        //Focus moved on to b.n; double-clicking a.n's group row routes
        //through the standalone path (NT_StandaloneFile) and brings the
        //existing tab back.
        tabCodes(window)->setCurrentIndex(1);
        QAbstractItemModel* model = solutionView(window)->model();
        const QModelIndex firstIndex =
            model->index(0, 0, model->index(0, 0));  // group child 0: a.n
        QCOMPARE(firstIndex.data().toString(), QString("a.n"));
        QMetaObject::invokeMethod(&window, "on_tvwSolution_doubleClicked",
                                  Q_ARG(QModelIndex, firstIndex));
        QCOMPARE(tabCodes(window)->count(), 2);
        QCOMPARE(tabCodes(window)->currentIndex(), 0);
    }

    void testTreeContextMenuOnStandaloneRow() {
        MainWindow window;
        QTemporaryDir dir;
        const QString path = QDir(dir.path()).filePath("solo.n");
        writeFile(path, kMainSource);
        inExec([&path] { acceptFileDialog(path); });
        act(window, "actOpenFile")->trigger();
        QAbstractItemModel* model = solutionView(window)->model();
        const QModelIndex fileIndex =
            model->index(0, 0, model->index(0, 0));
        solutionView(window)->setCurrentIndex(fileIndex);

        //activeMenu()'s three-level fallback (see the helper): a
        //synthetic QMenu::exec may register as neither active popup nor
        //active modal -- leaving it open would hang the test.
        inExec([] {
            QMenu* menu = activeMenu();
            QVERIFY2(menu != nullptr, "context menu must open");
            const QList<QAction*> actions = menu->actions();
            QVERIFY(actions.at(0)->isEnabled());    // Open
            QVERIFY(!actions.at(1)->isEnabled());   // Rename (F2)
            QVERIFY(!actions.at(2)->isEnabled());   // Remove
            menu->close();
        });
        const QPoint pos =
            solutionView(window)->visualRect(fileIndex).center();
        QMetaObject::invokeMethod(
            solutionView(window), "customContextMenuRequested",
            Q_ARG(QPoint, pos));
        //Drain the helper's singleShot even when the menu never opened,
        //so its failure lands on THIS test instead of the next one.
        QTest::qWait(1);
        QCOMPARE(tabCodes(window)->count(), 1);  // untouched
    }

    //--- layout ---

    void testDefaultLayoutFavorsEditor() {
        MainWindow window;
        MainWindow::applyDefaultLayout(window);
        window.resize(1000, 700);
        window.show();

        //sizes() (not width()) sidesteps frame/handle widths. The
        //solution column stays under a third of the editor pane; inside
        //the right column the editor keeps most of the vertical space.
        QSplitter* solutionSplitter =
            window.findChild<QSplitter*>("splitter");
        QSplitter* editorSplitter =
            window.findChild<QSplitter*>("splitter_2");
        QVERIFY(solutionSplitter != nullptr);
        QVERIFY(editorSplitter != nullptr);
        QVERIFY(solutionSplitter->sizes().at(0)
                < solutionSplitter->sizes().at(1) * 0.5);
        QVERIFY(editorSplitter->sizes().at(0)
                > editorSplitter->sizes().at(1) * 1.5);
    }

    void testLayoutRoundTripsThroughSettings() {
        QTemporaryDir dir;
        QSettings writer(QDir(dir.path()).filePath("layout.ini"),
                         QSettings::IniFormat);
        QSettings reader(QDir(dir.path()).filePath("layout.ini"),
                         QSettings::IniFormat);

        MainWindow window1;
        window1.resize(1000, 700);
        window1.show();
        QSplitter* solutionSplitter =
            window1.findChild<QSplitter*>("splitter");
        solutionSplitter->setSizes({300, 700});
        MainWindow::saveLayout(window1, writer);

        MainWindow window2;
        window2.resize(1000, 700);
        window2.show();
        QVERIFY(MainWindow::restoreLayout(window2, reader));
        QCOMPARE(window2.findChild<QSplitter*>("splitter")->sizes(),
                 solutionSplitter->sizes());
    }

    void testLayoutRestoreGarbageFallsBackToDefault() {
        QTemporaryDir dir;
        const QString iniPath = QDir(dir.path()).filePath("garbage.ini");
        writeFile(iniPath, "this is not splitter state");

        QSettings settings(iniPath, QSettings::IniFormat);
        MainWindow window;
        //A hidden splitter reports no meaningful sizes; show first so the
        //fallback proportions are measurable (same as the default-layout
        //test).
        window.resize(1000, 700);
        window.show();
        QVERIFY(!MainWindow::restoreLayout(window, settings));
        //The fallback is the editor-favoring default proportions.
        QSplitter* solutionSplitter =
            window.findChild<QSplitter*>("splitter");
        QVERIFY(solutionSplitter->sizes().at(0)
                < solutionSplitter->sizes().at(1) * 0.5);
    }

    //--- solution save ---

    void testSaveSolutionWritesFiles() {
        MainWindow window;
        QTemporaryDir dir;

        inExec([&] { acceptProjectDialog("App", dir.path()); });
        act(window, "actNewProject")->trigger();  // implicit solution
        inExec([&] { acceptNewFileDialog("main.n"); });
        act(window, "actAddNewFile")->trigger();

        const QString slnPath = QDir(dir.path()).filePath("IdeSln.nsln");
        inExec([&] { acceptFileDialog(slnPath); });
        act(window, "actSaveSolution")->trigger();

        QVERIFY(QFileInfo::exists(slnPath));
        QFile sln(slnPath);
        QVERIFY(sln.open(QIODevice::ReadOnly));
        QVERIFY(sln.readAll().contains("App.nproj"));
        const QString nprojPath = QDir(dir.path()).filePath("App.nproj");
        QVERIFY(QFileInfo::exists(nprojPath));
        QFile nproj(nprojPath);
        QVERIFY(nproj.open(QIODevice::ReadOnly));
        QVERIFY(nproj.readAll().contains("main.n"));
    }

    void testSaveAll() {
        MainWindow window;
        QTemporaryDir dir;

        inExec([&] { acceptProjectDialog("App", dir.path()); });
        act(window, "actNewProject")->trigger();
        inExec([&] { acceptNewFileDialog("main.n"); });
        act(window, "actAddNewFile")->trigger();
        currentCode(window)->appendPlainText("    int x = 1;");

        //One modal only: the unnamed solution asks for its .nsln path.
        //The editor has a path already, and saveSolution writes the
        //dirty project along.
        const QString slnPath = QDir(dir.path()).filePath("All.nsln");
        inExec([&] { acceptFileDialog(slnPath); });
        act(window, "actSaveAll")->trigger();

        QVERIFY(QFileInfo::exists(slnPath));
        QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath("App.nproj")));
        QCOMPARE(tabCodes(window)->tabText(0), QString("main.n"));  // saved
    }

    //--- build & run (real ncc + nvm) ---

    void testBuildAndRun() {
        MainWindow window;
        QTemporaryDir dir;

        inExec([&] { acceptProjectDialog("App", dir.path()); });
        act(window, "actNewProject")->trigger();
        inExec([&] { acceptNewFileDialog("main.n"); });
        act(window, "actAddNewFile")->trigger();

        //Type the program; the build must save the dirty editor and the
        //never-saved .nproj before invoking ncc.
        currentCode(window)->setPlainText(kMainSource);
        act(window, "actBuild")->trigger();

        QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath("App.nproj")));
        //The status summarizes the ncc run (asserted before the product
        //check so a failed start names the wrong message, not just a
        //missing file).
        QCOMPARE(window.statusBar()->currentMessage(),
                 QString("Build succeeded"));
        const QString nmodPath = QDir(dir.path()).filePath("App.nmod");
        QVERIFY(QFileInfo::exists(nmodPath));

        //Run the built module: nvm propagates main's return value.
        act(window, "actStartRunning")->trigger();
        QTextBrowser* executeOut =
            window.findChild<QTextBrowser*>("txtExecuteOut");
        QVERIFY(QTest::qWaitFor([&] {
            return executeOut->toPlainText()
                .contains("Program exited with code 42");
        }, 15000));
        QVERIFY(act(window, "actStartRunning")->isEnabled());
        QVERIFY(!act(window, "actStopRunning")->isEnabled());
    }

    void testRunWithoutBuildWarns() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());  // no .nmod on disk

        inExec([&] { answerMessageBox(QMessageBox::Ok); });
        act(window, "actStartRunning")->trigger();

        //The warning aborted the run before anything was touched.
        QCOMPARE(window.findChild<QTextBrowser*>("txtExecuteOut")
                     ->toPlainText(),
                 QString());
    }

    void testBuildFailingSource() {
        MainWindow window;
        QTemporaryDir dir;

        inExec([&] { acceptProjectDialog("App", dir.path()); });
        act(window, "actNewProject")->trigger();
        inExec([&] { acceptNewFileDialog("main.n"); });
        act(window, "actAddNewFile")->trigger();

        currentCode(window)->setPlainText(
            "public int main() {\n"
            "    return undefined_name;\n"
            "}\n");
        act(window, "actBuild")->trigger();

        //ncc ran and exited nonzero: the failed-exit path of the status
        //line, plus a diagnostic the log browser could navigate.
        QCOMPARE(window.statusBar()->currentMessage(),
                 QString("Build failed"));
        QVERIFY(window.findChild<CompileLogBrowser*>("txtCompileOut")
                    ->toPlainText()
                    .contains("Error"));
        QVERIFY(!QFileInfo::exists(
            QDir(dir.path()).filePath("App.nmod")));
    }

    void testStopRunningKillsProcess() {
        MainWindow window;
        QTemporaryDir dir;

        inExec([&] { acceptProjectDialog("App", dir.path()); });
        act(window, "actNewProject")->trigger();
        inExec([&] { acceptNewFileDialog("main.n"); });
        act(window, "actAddNewFile")->trigger();

        currentCode(window)->setPlainText(
            "public int main() {\n"
            "    while (1 < 2) {\n"
            "    }\n"
            "    return 0;\n"
            "}\n");
        act(window, "actBuild")->trigger();
        QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath("App.nmod")));

        //runProject's waitForStarted is synchronous: past the trigger,
        //the process is either running or was never started.
        act(window, "actStartRunning")->trigger();
        QVERIFY(act(window, "actStopRunning")->isEnabled());
        QVERIFY(!act(window, "actStartRunning")->isEnabled());

        act(window, "actStopRunning")->trigger();
        //kill() ends the process asynchronously; finished() then
        //reports the crash and updateMenuState restores Start.
        QTextBrowser* executeOut =
            window.findChild<QTextBrowser*>("txtExecuteOut");
        QVERIFY(QTest::qWaitFor([&] {
            return executeOut->toPlainText()
                .contains("The process crashed.");
        }, 15000));
        QVERIFY(act(window, "actStartRunning")->isEnabled());
        QVERIFY(!act(window, "actStopRunning")->isEnabled());
    }

    void testClearBuild() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());

        window.findChild<CompileLogBrowser*>("txtCompileOut")
            ->append("stale build output");
        window.findChild<QTextBrowser*>("txtExecuteOut")
            ->append("stale run output");

        act(window, "actClearBuild")->trigger();

        QCOMPARE(window.findChild<CompileLogBrowser*>("txtCompileOut")
                     ->toPlainText(),
                 QString());
        QCOMPARE(window.findChild<QTextBrowser*>("txtExecuteOut")
                     ->toPlainText(),
                 QString());
    }

    //--- compile-log navigation ---

    void testCompileLogNavigation() {
        MainWindow window;
        QTemporaryDir dir;
        openFixtureProject(window, dir.path());
        const QString mainPath = QDir(dir.path()).filePath("App/main.n");

        CompileLogItemInfo info;
        info.filePath = mainPath;
        info.line = 2;      // "    return 42;"
        info.column = 3;
        info.message = "Error: boom";
        CompileLogBrowser* log =
            window.findChild<CompileLogBrowser*>("txtCompileOut");
        //moc records the signal's parameter type as written (without
        //the namespace), so the Q_ARG spelling must match that.
        QMetaObject::invokeMethod(log, "lineSelected",
                                  Q_ARG(CompileLogItemInfo, info));

        //The editor opened at the error site (QTextCursor is 0-based).
        QCOMPARE(tabCodes(window)->count(), 1);
        QCOMPARE(currentCode(window)->textCursor().blockNumber(), 1);
        QCOMPARE(currentCode(window)->textCursor().columnNumber(), 2);
        QCOMPARE(window.statusBar()->currentMessage(),
                 QString("Error: boom"));
    }

    //--- close event ---

    void testCloseEventPromptsOnDirtyEditor() {
        QTemporaryDir dir;

        //Cancel on the FIRST prompt (the unsaved solution: the opened
        //project was never saved into a .nsln) keeps the window open.
        {
            MainWindow window;
            window.show();
            openFixtureProject(window, dir.path());
            QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
                Q_ARG(QModelIndex, firstFileIndex(window)));
            currentCode(window)->appendPlainText("    int x = 1;");

            inExec([&] { answerMessageBox(QMessageBox::Cancel); });
            window.close();
            QVERIFY(window.isVisible());
        }

        //Discard answers BOTH prompts (solution, then the dirty editor)
        //and the window closes.
        {
            MainWindow window;
            window.show();
            openFixtureProject(window, dir.path());
            QMetaObject::invokeMethod(solutionView(window), "doubleClicked",
                Q_ARG(QModelIndex, firstFileIndex(window)));
            currentCode(window)->appendPlainText("    int x = 1;");

            inExecSteps2([&] { answerMessageBox(QMessageBox::Discard); },
                         [&] { answerMessageBox(QMessageBox::Discard); });
            window.close();
            QVERIFY(!window.isVisible());
            QCOMPARE(tabCodes(window)->count(), 0);
        }
    }

    //--- icons (Step 8) ---

    void testActionAndTreeIcons() {
        MainWindow window;

        //The 14 ported actions carry EN's icons. Force a real pixmap
        //load -- QIcon(path).isNull() stays false even for a missing
        //file (the engine is lazy), a null pixmap does not.
        const char* const iconActions[] = {
            "actNewFile",     "actOpenFile",      "actSaveFile",
            "actNewProject",  "actOpenProject",   "actAddExistFile",
            "actAddNewFile",  "actRemoveFile",    "actProjectProp",
            "actBuild",       "actStartRunning",  "actStopRunning",
            "actSaveAll",     "actSaveProject",
        };
        for (const char* name : iconActions) {
            QVERIFY2(!act(window, name)->icon()
                          .pixmap(QSize(16, 16))
                          .isNull(),
                     name);
        }

        //The solution tree decorates its three node kinds.
        QTemporaryDir dir;
        const QModelIndex projectIndex =
            openFixtureProject(window, dir.path());
        QAbstractItemModel* model = solutionView(window)->model();
        const QModelIndex solutionIndex = projectIndex.parent();
        const QModelIndex fileIndex =
            model->index(0, 0, projectIndex);
        for (const QModelIndex& index :
             {solutionIndex, projectIndex, fileIndex}) {
            QVERIFY(!index.data(Qt::DecorationRole)
                         .value<QIcon>()
                         .pixmap(QSize(16, 16))
                         .isNull());
        }
    }

    //--- translations (Step 8) ---

    void testInstallTranslations() {
        QApplication* app = static_cast<QApplication*>(qApp);

        //zh_CN: the English-authored code strings flip to Chinese.
        //Escapes keep this file pure ASCII (MSVC reads BOM-less
        //sources in the system codepage): 构建成功 is
        //"Build succeeded" in Chinese, 就绪 is "Ready".
        QTranslator* zh = nlang::installTranslations(app, QLocale("zh_CN"));
        QVERIFY(zh != nullptr);
        QCOMPARE(MainWindow::tr("Build succeeded"),
                 QString::fromUtf16(u"\u6784\u5EFA\u6210\u529F"));
        QCOMPARE(MainWindow::tr("Ready"),
                 QString::fromUtf16(u"\u5C31\u7EEA"));
        //Default names stay untranslated so they match the (also
        //untranslated) default file names.
        QCOMPARE(MainWindow::tr("Solution1"), QString("Solution1"));
        qApp->removeTranslator(zh);
        delete zh;

        //en_US: the Chinese-authored .ui strings flip to English
        //(uic-generated setupUi translates in the "MainWindow"
        //context; 文件(&F) is the menu title "文件(&F)").
        QTranslator* en = nlang::installTranslations(app, QLocale("en_US"));
        QVERIFY(en != nullptr);
        QCOMPARE(QApplication::translate("MainWindow",
                                         u8"\u6587\u4EF6(&F)"),
                 QString("&File"));
        qApp->removeTranslator(en);
        delete en;

        //A locale without a catalog installs nothing; the authored
        //strings show through unchanged.
        QVERIFY(nlang::installTranslations(app, QLocale("fr_FR"))
                == nullptr);
        QCOMPARE(MainWindow::tr("Build succeeded"),
                 QString("Build succeeded"));
    }

    //--- view menu ---

    void testViewToggles() {
        MainWindow window;
        window.show();

        QTabWidget* output = window.findChild<QTabWidget*>("tabOutput");
        QAction* viewOutput = act(window, "actViewOutput");
        QVERIFY(viewOutput->isChecked());
        QVERIFY(output->isVisible());

        viewOutput->trigger();
        QVERIFY(!output->isVisible());
        viewOutput->trigger();
        QVERIFY(output->isVisible());

        //Hiding the dock via its close button un-checks the view action.
        QDockWidget* dock = window.findChild<QDockWidget*>("dckSolution");
        QVERIFY(act(window, "actViewSolution")->isChecked());
        dock->hide();
        QVERIFY(!act(window, "actViewSolution")->isChecked());
    }

    //--- help menu ---

    void testHelpMenuOpensOneWindowPerDocument() {
        MainWindow window;
        act(window, "actHelpLanguageSpec")->trigger();
        act(window, "actHelpGettingStarted")->trigger();
        QCOMPARE(window.findChildren<HelpWindow*>().size(), 2);
        const QList<HelpWindow*> windows =
            window.findChildren<HelpWindow*>();
        for (HelpWindow* helpWindow : windows)
            QVERIFY(helpWindow->documentLoaded());
    }

    void testHelpMenuReopensRaiseExistingWindow() {
        MainWindow window;
        act(window, "actHelpVmArch")->trigger();
        act(window, "actHelpVmArch")->trigger();
        QCOMPARE(window.findChildren<HelpWindow*>().size(), 1);
        QVERIFY(window.findChild<HelpWindow*>()->documentLoaded());
    }

    //Identity of the reopened window is NOT asserted by pointer: the
    //allocator may legally reuse the freed address (ABA). Destruction is
    //proven by the QTRY isEmpty below; without WA_DeleteOnClose the
    //window would survive it.
    void testHelpWindowCloseThenReopenCreatesFresh() {
        MainWindow window;
        act(window, "actHelpVmArch")->trigger();
        HelpWindow* first = window.findChildren<HelpWindow*>().at(0);
        QVERIFY(first != nullptr);
        first->close();  // closeEvent -> reject(); same delete path as Esc
        QTRY_VERIFY(window.findChildren<HelpWindow*>().isEmpty());
        act(window, "actHelpVmArch")->trigger();
        QCOMPARE(window.findChildren<HelpWindow*>().size(), 1);
        QVERIFY(window.findChildren<HelpWindow*>().at(0)->documentLoaded());
    }

    //Esc dismisses a QDialog via reject() -> done(), and done() runs the
    //same WA_DeleteOnClose handling as close() (QDialogPrivate::hide
    //calls close_helper, Qt 5.15 qdialog.cpp): the window is destroyed
    //either way and the menu action then opens a fresh one. The event
    //loop spin below mirrors the real user gap in which deleteLater runs.
    //Fresh-object identity is NOT asserted by pointer: the allocator may
    //reuse the freed address. The QTRY isEmpty above is the load-bearing
    //check (without WA_DeleteOnClose the window would survive it).
    void testHelpWindowEscDismissThenReopenCreatesFresh() {
        MainWindow window;
        act(window, "actHelpGettingStarted")->trigger();
        HelpWindow* first = window.findChildren<HelpWindow*>().at(0);
        QVERIFY(first != nullptr);
        first->reject();
        QVERIFY(!first->isVisible());
        QTRY_VERIFY(window.findChildren<HelpWindow*>().isEmpty());
        act(window, "actHelpGettingStarted")->trigger();
        QCOMPARE(window.findChildren<HelpWindow*>().size(), 1);
        QVERIFY(window.findChildren<HelpWindow*>().at(0)->isVisible());
        QVERIFY(window.findChildren<HelpWindow*>().at(0)->documentLoaded());
    }

    //--- user journey (Step 10) ---

    //The plan's IDE checklist walked as ONE continuous session: new
    //solution -> new project -> new file -> edit & save -> syntax
    //highlight -> build -> run -> error navigation. The earlier tests
    //cover each slice in isolation; this one proves they compose
    //through the real actions, dialogs, ncc and nvm.
    void testUserJourneyE2E() {
        MainWindow window;
        QTemporaryDir dir;

        //New solution: the tree roots at Solution1.
        act(window, "actNewSolution")->trigger();
        QAbstractItemModel* model = solutionView(window)->model();
        QCOMPARE(model->index(0, 0).data().toString(),
                 QString("Solution1"));

        //New project through the real properties dialog.
        inExec([&] { acceptProjectDialog("App", dir.path()); });
        act(window, "actNewProject")->trigger();
        const QModelIndex projectIndex =
            model->index(0, 0, model->index(0, 0));
        QCOMPARE(projectIndex.data().toString(), QString("App"));

        //New file through the real new-file dialog; it opens in the
        //editor.
        inExec([&] { acceptNewFileDialog("main.n"); });
        act(window, "actAddNewFile")->trigger();
        CodeEditor* editor = currentCode(window);
        QCOMPARE(tabCodes(window)->tabText(0), QString("main.n"));

        //Edit and explicitly save: the typed program reaches the disk.
        editor->setPlainText(kMainSource);
        act(window, "actSaveFile")->trigger();
        //Scoped so the read handle closes at the brace: the next Build
        //auto-saves the then-dirty editor atomically (QSaveFile renames
        //over the target), and Windows refuses that rename while any
        //handle holds the file open.
        {
            QFile saved(QDir(dir.path()).filePath("main.n"));
            QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
            QCOMPARE(QString::fromUtf8(saved.readAll()),
                     QString(kMainSource));
        }

        //The compiler's lexer colored the program: keyword blue,
        //number dark cyan.
        QCOMPARE(colorAt(editor->document()->firstBlock(), 0),
                 QColor(Qt::blue));                          // "public"
        const QTextBlock returnBlock =
            editor->document()->firstBlock().next();
        QCOMPARE(colorAt(returnBlock, 4), QColor(Qt::blue));      // "return"
        QCOMPARE(colorAt(returnBlock, 11), QColor(Qt::darkCyan)); // "42"

        //Build: ncc really ran (module on disk).
        act(window, "actBuild")->trigger();
        QCOMPARE(window.statusBar()->currentMessage(),
                 QString("Build succeeded"));
        QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath("App.nmod")));

        //Run: nvm propagates main's exit code to the output page.
        act(window, "actStartRunning")->trigger();
        QTextBrowser* executeOut =
            window.findChild<QTextBrowser*>("txtExecuteOut");
        QVERIFY(QTest::qWaitFor([&] {
            return executeOut->toPlainText()
                .contains("Program exited with code 42");
        }, 15000));

        //Break the source and rebuild: the diagnostic lands in the log
        //browser, and double-clicking that line opens the error site.
        editor->setPlainText(
            "public int main() {\n"
            "    return undefined_name;\n"
            "}\n");
        act(window, "actBuild")->trigger();
        QCOMPARE(window.statusBar()->currentMessage(),
                 QString("Build failed"));
        CompileLogBrowser* log =
            window.findChild<CompileLogBrowser*>("txtCompileOut");
        QVERIFY(log->toPlainText().contains("Error"));

        //A real double-click on the diagnostic line. Lazy layout means
        //no valid geometry before resize + adjustSize (the un-shown
        //dock never ran a layout), and the click targets the viewport
        //with viewport-relative coordinates, as QTextEdit's mouse
        //handlers expect.
        log->resize(600, 200);
        log->document()->adjustSize();
        const QTextCursor diagnostic(
            log->document()->find("(line ").block());
        QVERIFY(!diagnostic.isNull());
        log->setTextCursor(diagnostic);
        QTest::mouseDClick(log->viewport(), Qt::LeftButton, Qt::NoModifier,
                           log->cursorRect(diagnostic).center());
        QCOMPARE(editor->textCursor().blockNumber(), 1);
    }
};

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
