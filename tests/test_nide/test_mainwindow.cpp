/*--- test_mainwindow.cpp - MainWindow integration tests ---*/
#include "MainWindow.h"
#include "CodeEditor.h"
#include "CompileLogBrowser.h"
#include "FileEditor.h"
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
#include <QLineEdit>
#include <QMessageBox>
#include <QStatusBar>
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
//directory field keeps its per-project default).
void acceptNewFileDialog(const QString& fileName) {
    QDialog* dialog =
        qobject_cast<QDialog*>(QApplication::activeModalWidget());
    QLineEdit* nameEdit =
        dialog != nullptr ? dialog->findChild<QLineEdit*>("edtName")
                          : nullptr;
    if (nameEdit != nullptr) {
        nameEdit->setText(fileName);
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
