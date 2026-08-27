/*--- test_dialogs.cpp - NewFileDialog / ProjectPropDialog unit tests ---*/
#include "NewFileDialog.h"
#include "ProjectModel.h"
#include "ProjectPropDialog.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace nlang;

namespace {

//Schedule an action to run inside the dialog's modal exec() loop --
//dialog logic under test cannot be driven any other way.
template<typename Fn>
void inExec(Fn action) {
    QTimer::singleShot(0, action);
}

//Drive a sequence of actions through CONSECUTIVE modal loops (dialog
//-> warning box -> retry dialog). Each step is queued from inside the
//previous one: a zero-timeout timer created during an event batch
//fires in the NEXT batch, i.e. in the next modal loop. Queueing all
//steps up front instead collapses the whole sequence into the FIRST
//exec(): already-expired timers keep delivering in the same
//processEvents pass even after accept() set the loop's exit flag, so
//the warning and retry loops would never run (observed with a loop-
//iteration probe: one iteration, test still green).
template<typename Fn1, typename Fn2, typename Fn3>
void inExecSteps(Fn1 step1, Fn2 step2, Fn3 step3) {
    QTimer::singleShot(0, [step1, step2, step3]() mutable {
        step1();
        QTimer::singleShot(0, [step2, step3]() mutable {
            step2();
            QTimer::singleShot(0, step3);
        });
    });
}

//Auto-reject any modal warning the dialog pops up (createProject's
//retry prompts). A no-op when the active modal is the dialog itself.
void dismissWarnings() {
    if (QWidget* modal = QApplication::activeModalWidget()) {
        if (QMessageBox* box = qobject_cast<QMessageBox*>(modal))
            box->reject();
    }
}

//Widget lookup by uic objectName. Null-derefs a missing name loudly
//enough for a test; QVERIFY2 cannot live here (its early `return;`
//does not compile in a value-returning function).
QLineEdit* edit(QWidget* dialog, const char* objectName) {
    return dialog->findChild<QLineEdit*>(objectName);
}

QPushButton* button(QWidget* dialog, const char* objectName) {
    return dialog->findChild<QPushButton*>(objectName);
}

} // namespace

class TestDialogs : public QObject {
    Q_OBJECT

private slots:
    //--- NewFileDialog ---

    void testInitDefaultsWithoutProject() {
        NewFileDialog dialog;
        dialog.init();

        QCOMPARE(edit(&dialog, "edtName")->text(), QString("Untitled.n"));
        QCOMPARE(edit(&dialog, "edtDirectory")->text(), QDir::currentPath());
        //The stem is selected so typing replaces it, keeping ".n".
        QCOMPARE(edit(&dialog, "edtName")->selectedText(), QString("Untitled"));
        QVERIFY(button(&dialog, "btnSubmit")->isEnabled());
    }

    void testInitDefaultsWithProject() {
        QTemporaryDir dir;
        ProjectNode project("App", dir.path());
        NewFileDialog dialog;
        dialog.init(&project);

        //NLang sources live next to the .nproj -- not in a src/ child
        //directory as EN assumed.
        QCOMPARE(edit(&dialog, "edtDirectory")->text(), dir.path());
    }

    void testSubmitNeedsNameAndDirectory() {
        NewFileDialog dialog;
        dialog.init();

        edit(&dialog, "edtName")->clear();
        QVERIFY(!button(&dialog, "btnSubmit")->isEnabled());

        edit(&dialog, "edtName")->setText("main.n");
        QVERIFY(button(&dialog, "btnSubmit")->isEnabled());
    }

    void testGetFilePathAccept() {
        NewFileDialog dialog;
        dialog.init();
        inExec([&] { dialog.accept(); });

        QString filePath;
        QVERIFY(dialog.getFilePath(filePath));
        QCOMPARE(filePath,
                 QFileInfo(QDir::currentPath(), "Untitled.n").absoluteFilePath());
    }

    void testGetFilePathReject() {
        NewFileDialog dialog;
        dialog.init();
        inExec([&] { dialog.reject(); });

        QString filePath = "untouched";
        QVERIFY(!dialog.getFilePath(filePath));
        QCOMPARE(filePath, QString("untouched"));
    }

    //--- ProjectPropDialog: create ---

    void testCreateDefaults() {
        SolutionNode solution("Sln");
        ProjectPropDialog dialog;
        QString defaultName, defaultDir;
        inExec([&] {
            defaultName = edit(&dialog, "edtProjectName")->text();
            defaultDir = edit(&dialog, "edtProjectDir")->text();
            dialog.reject();
        });

        QVERIFY(dialog.createProject(solution) == nullptr);
        QCOMPARE(defaultName, QString("Project1"));
        QCOMPARE(defaultDir, QDir::currentPath());
        QCOMPARE(solution.projectCount(), 0);
    }

    void testCreateAcceptAddsProject() {
        QTemporaryDir dir;
        SolutionNode solution("Sln");
        ProjectPropDialog dialog;
        inExec([&] {
            edit(&dialog, "edtProjectName")->setText("App");
            edit(&dialog, "edtProjectDir")->setText(dir.path());
            edit(&dialog, "edtNamespace")->setText("app");
            edit(&dialog, "edtOutputDir")->setText("bin");
            edit(&dialog, "edtIntermediateDir")->setText("obj");
            dialog.accept();
        });

        ProjectNode* project = dialog.createProject(solution);
        QVERIFY(project != nullptr);
        QCOMPARE(solution.projectCount(), 1);
        QCOMPARE(project->name(), QString("App"));
        QCOMPARE(project->projectDir(), QDir(dir.path()).absolutePath());
        QCOMPARE(project->namespace_(), QString("app"));
        QCOMPARE(project->outputDir(), QString("bin"));
        QCOMPARE(project->intermediateDir(), QString("obj"));
        QVERIFY(project->isDirty());
    }

    void testCreateRejectReturnsNull() {
        SolutionNode solution("Sln");
        ProjectPropDialog dialog;
        inExec([&] { dialog.reject(); });

        QVERIFY(dialog.createProject(solution) == nullptr);
        QCOMPARE(solution.projectCount(), 0);
    }

    void testCreateExistingProjectFileNotOverwritten() {
        QTemporaryDir dir;
        QFile existing(QDir(dir.path()).filePath("App.nproj"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("<Project/>");
        existing.close();

        SolutionNode solution("Sln");
        ProjectPropDialog dialog;
        inExecSteps(
            [&] {
                edit(&dialog, "edtProjectName")->setText("App");
                edit(&dialog, "edtProjectDir")->setText(dir.path());
                dialog.accept();
            },
            &dismissWarnings,   // warning shows: file already exists
            [&] { dialog.reject(); });   // user gives up

        QVERIFY(dialog.createProject(solution) == nullptr);
        QCOMPARE(solution.projectCount(), 0);
    }

    void testCreateDuplicateProjectRejected() {
        QTemporaryDir dir;
        SolutionNode solution("Sln");
        //The path is already in the solution (file need not exist).
        QVERIFY(solution.addProject(
            QDir(dir.path()).filePath("App.nproj")) != nullptr);

        ProjectPropDialog dialog;
        inExecSteps(
            [&] {
                edit(&dialog, "edtProjectName")->setText("App");
                edit(&dialog, "edtProjectDir")->setText(dir.path());
                dialog.accept();
            },
            &dismissWarnings,   // warning shows: already in solution
            [&] { dialog.reject(); });

        QVERIFY(dialog.createProject(solution) == nullptr);
        QCOMPARE(solution.projectCount(), 1);
    }

    //The retry loop's core property: initForCreate() runs once, before
    //the loop, so the user's entries survive a warning and a corrected
    //accept succeeds (a regression re-initializing on retry wipes them).
    void testCreateRetrySucceedsWithCorrectedDirectory() {
        QTemporaryDir takenDir;
        QTemporaryDir cleanDir;
        QFile existing(QDir(takenDir.path()).filePath("App.nproj"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.close();

        SolutionNode solution("Sln");
        ProjectPropDialog dialog;
        inExecSteps(
            [&] {
                edit(&dialog, "edtProjectName")->setText("App");
                edit(&dialog, "edtProjectDir")->setText(takenDir.path());
                edit(&dialog, "edtNamespace")->setText("app");
                dialog.accept();
            },
            &dismissWarnings,   // warning shows: file already exists
            [&] {               // user corrects the location, retries
                edit(&dialog, "edtProjectDir")->setText(cleanDir.path());
                dialog.accept();
            });

        ProjectNode* project = dialog.createProject(solution);
        QVERIFY(project != nullptr);
        //The name/namespace come from the FIRST entry: only a loop that
        //keeps the user's edits across the retry produces them (a re-init
        //would reset the name to "Project1" and the namespace to "").
        QCOMPARE(project->name(), QString("App"));
        QCOMPARE(project->namespace_(), QString("app"));
        QCOMPARE(project->projectDir(), QDir(cleanDir.path()).absolutePath());
        QCOMPARE(solution.projectCount(), 1);
    }

    void testCreateNameWithPathSeparatorRejected() {
        QTemporaryDir dir;
        SolutionNode solution("Sln");
        ProjectPropDialog dialog;
        inExecSteps(
            [&] {
                edit(&dialog, "edtProjectName")->setText("a/b");
                edit(&dialog, "edtProjectDir")->setText(dir.path());
                dialog.accept();
            },
            &dismissWarnings,   // warning shows: separators rejected
            [&] { dialog.reject(); });

        QVERIFY(dialog.createProject(solution) == nullptr);
        QCOMPARE(solution.projectCount(), 0);
    }

    //--- ProjectPropDialog: edit ---

    void testEditAppliesFields() {
        QTemporaryDir dir;
        ProjectNode project("App", dir.path());
        project.setNamespace("old");
        project.setOutputDir("oldBin");
        project.setIntermediateDir("oldObj");
        project.clearDirty();

        ProjectPropDialog dialog;
        inExec([&] {
            edit(&dialog, "edtNamespace")->setText("new");
            edit(&dialog, "edtOutputDir")->setText("newBin");
            edit(&dialog, "edtIntermediateDir")->setText("newObj");
            dialog.accept();
        });

        QVERIFY(dialog.editProject(project));
        QCOMPARE(project.namespace_(), QString("new"));
        QCOMPARE(project.outputDir(), QString("newBin"));
        QCOMPARE(project.intermediateDir(), QString("newObj"));
        //Unlike EN, the editable namespace field is actually applied.
        QVERIFY(project.isDirty());
    }

    void testEditRejectKeepsFields() {
        QTemporaryDir dir;
        ProjectNode project("App", dir.path());
        project.setNamespace("keep");
        project.clearDirty();

        ProjectPropDialog dialog;
        inExec([&] { dialog.reject(); });

        QVERIFY(!dialog.editProject(project));
        QCOMPARE(project.namespace_(), QString("keep"));
    }

    void testEditLocksNameAndLocation() {
        QTemporaryDir dir;
        ProjectNode project("App", dir.path());
        ProjectPropDialog dialog;
        bool nameReadOnly = false, dirReadOnly = false, browseEnabled = true;
        inExec([&] {
            nameReadOnly = edit(&dialog, "edtProjectName")->isReadOnly();
            dirReadOnly = edit(&dialog, "edtProjectDir")->isReadOnly();
            browseEnabled = button(&dialog, "btnProjectDir")->isEnabled();
            dialog.reject();
        });

        QVERIFY(!dialog.editProject(project));
        QVERIFY(nameReadOnly);
        QVERIFY(dirReadOnly);
        QVERIFY(!browseEnabled);
    }
};

QTEST_MAIN(TestDialogs)
#include "test_dialogs.moc"
