/*--- MainWindowBuildRun.cpp - build and run pipeline of the NLang IDE
    main window: project/standalone builds, process run, compile-log
    navigation. ---*/
#include "MainWindow.h"
#include "CodeEditor.h"
#include "CompileLogBrowser.h"
#include "FileEditor.h"
#include "ProjectModel.h"
#include "SolutionTreeModel.h"

#include "ui_MainWindow.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QTextBlock>
#include <QTextCursor>

namespace nlang {

//--- build / run ---

void MainWindow::on_actBuild_triggered() {
    ProjectNode* project = currentProject();
    if (project != nullptr) {
        buildProject(*project);
        return;
    }
    const QString standalone = currentStandaloneTarget();
    if (!standalone.isEmpty())
        buildStandaloneFile(standalone);
}

bool MainWindow::buildProject(ProjectNode& project) {
    //Save the editors of this project's files so ncc sees the edits.
    for (const auto& file : project.files()) {
        FileEditor* editor = m_editors.find(file->absolutePath());
        if (editor != nullptr && editor->dirty() &&
            !saveEditor(editor))
            return false;
    }
    if (project.isDirty() && !saveProject(project))
        return false;

    m_ui->txtCompileOut->setProject(&project);
    m_ui->txtCompileOut->clear();
    showOutputPage(m_ui->tabCompileOut);

    const QString output = outputFilePath(project);
    if (!QDir().mkpath(QFileInfo(output).absolutePath())) {
        QMessageBox::warning(
            this, tr("Error"),
            tr("Cannot create the output directory '%1'.")
                .arg(QFileInfo(output).absolutePath()));
        return false;
    }

    //Synchronous build: ncc writes diagnostics in the
    //shape CompileLogBrowser parses -- on stderr, so merge the channels
    //before reading (plain readAll() would only see stdout).
    QProcess ncc(this);
    ncc.setProcessChannelMode(QProcess::MergedChannels);
    ncc.setWorkingDirectory(project.projectDir());
    ncc.start(toolPath("ncc"),
              {"build", "-p", projectFilePath(&project), "-o", output});
    QString log;
    bool succeeded = false;
    if (!ncc.waitForStarted(-1)) {
        log = tr("Failed to start '%1'.").arg(toolPath("ncc"));
    } else {
        ncc.waitForFinished(-1);
        log = QString::fromLocal8Bit(ncc.readAll());
        //Read the exit state only after a real run: start() resets
        //exitCode/exitStatus, so the failed-start path would fake
        //success if it shared this expression.
        succeeded = ncc.exitStatus() == QProcess::NormalExit
            && ncc.exitCode() == 0;
    }
    m_ui->txtCompileOut->append(log);
    m_ui->statusBar->showMessage(
        succeeded ? tr("Build succeeded") : tr("Build failed"));
    return succeeded;
}

void MainWindow::runProject(ProjectNode& project) {
    if (m_executed.state() != QProcess::NotRunning)
        return;
    const QString output = outputFilePath(project);
    if (!QFileInfo::exists(output)) {
        QMessageBox::warning(
            this, tr("Error"),
            tr("'%1' does not exist. Build the project first.").arg(output));
        return;
    }
    m_ui->txtExecuteOut->clear();
    showOutputPage(m_ui->tabExecuteOut);
    m_executed.setWorkingDirectory(project.projectDir());
    m_executed.start(toolPath("nvm"), {output});
    if (!m_executed.waitForStarted(-1)) {
        m_ui->txtExecuteOut->append(
            tr("Failed to start '%1'.").arg(toolPath("nvm")));
        return;
    }
    updateMenuState();  // Running now: Start off, Stop on
}

void MainWindow::on_actStartRunning_triggered() {
    ProjectNode* project = currentProject();
    if (project != nullptr) {
        runProject(*project);
        return;
    }
    const QString standalone = currentStandaloneTarget();
    if (!standalone.isEmpty())
        runStandaloneFile(standalone);
}

QString MainWindow::currentStandaloneTarget() const {
    SolutionTreeItem* item =
        m_solutionTree->itemAt(m_ui->tvwSolution->currentIndex());
    if (item != nullptr &&
        item->nodeType() == SolutionTreeItem::NT_StandaloneFile)
        return item->standalonePath();
    if (currentProject() != nullptr)
        return QString();  // the project target wins (spec 3.4)
    //Tree points at no project/standalone row: fall back to the active
    //editor when no project tracks its file.
    FileEditor* editor = currentEditor();
    if (editor != nullptr &&
        findFileNodeByPath(editor->filePath()) == nullptr)
        return editor->filePath();
    return QString();
}

bool MainWindow::buildStandaloneFile(const QString& filePath) {
    //The target's editor must not sit on unsaved edits (buildProject
    //saves the project's files for the same reason).
    FileEditor* editor = m_editors.find(filePath);
    if (editor != nullptr && editor->dirty() && !saveEditor(editor))
        return false;

    //No ProjectNode to resolve relative paths in diagnostics.
    m_ui->txtCompileOut->setProject(nullptr);
    m_ui->txtCompileOut->clear();
    showOutputPage(m_ui->tabCompileOut);

    const QString output = standaloneNmodPath(filePath);
    QProcess ncc(this);
    ncc.setProcessChannelMode(QProcess::MergedChannels);
    ncc.setWorkingDirectory(QFileInfo(filePath).absolutePath());
    ncc.start(toolPath("ncc"), {"build", filePath, "-o", output});
    QString log;
    bool succeeded = false;
    if (!ncc.waitForStarted(-1)) {
        log = tr("Failed to start '%1'.").arg(toolPath("ncc"));
    } else {
        ncc.waitForFinished(-1);
        log = QString::fromLocal8Bit(ncc.readAll());
        //Read the exit state only after a real run (same trap as
        //buildProject: a failed start would fake success here).
        succeeded = ncc.exitStatus() == QProcess::NormalExit
            && ncc.exitCode() == 0;
    }
    m_ui->txtCompileOut->append(log);
    m_ui->statusBar->showMessage(
        succeeded ? tr("Build succeeded") : tr("Build failed"));
    return succeeded;
}

void MainWindow::runStandaloneFile(const QString& filePath) {
    if (m_executed.state() != QProcess::NotRunning)
        return;
    //A dirty editor must not run as the stale on-disk build.
    FileEditor* editor = m_editors.find(filePath);
    if (editor != nullptr && editor->dirty() && !saveEditor(editor))
        return;
    const QString output = standaloneNmodPath(filePath);
    //D2: unlike the project Run (which asks for a manual build first),
    //a missing/outdated module is rebuilt here automatically.
    if (!QFileInfo::exists(output) ||
        QFileInfo(output).lastModified() <
            QFileInfo(filePath).lastModified()) {
        if (!buildStandaloneFile(filePath))
            return;
    }
    m_ui->txtExecuteOut->clear();
    showOutputPage(m_ui->tabExecuteOut);
    //Run from the module's dir: an example writing files stays inside
    //the temp area (never the install dir); no example needs the
    //source dir as CWD (the e2e suite proves that).
    m_executed.setWorkingDirectory(QFileInfo(output).absolutePath());
    m_executed.start(toolPath("nvm"), {output});
    if (!m_executed.waitForStarted(-1)) {
        m_ui->txtExecuteOut->append(
            tr("Failed to start '%1'.").arg(toolPath("nvm")));
        return;
    }
    updateMenuState();  // Running now: Start off, Stop on
}

void MainWindow::on_actStopRunning_triggered() {
    if (m_executed.state() != QProcess::NotRunning)
        m_executed.kill();
}

void MainWindow::on_actClearBuild_triggered() {
    m_ui->txtCompileOut->clear();
    m_ui->txtExecuteOut->clear();
}

void MainWindow::onExecOutput() {
    m_ui->txtExecuteOut->append(
        QString::fromLocal8Bit(m_executed.readAllStandardOutput()));
}

void MainWindow::onExecFinished(int exitCode, QProcess::ExitStatus status) {
    if (status == QProcess::CrashExit)
        m_ui->txtExecuteOut->append(tr("The process crashed."));
    else
        m_ui->txtExecuteOut->append(
            tr("Program exited with code %1.").arg(exitCode));
    updateMenuState();  // NotRunning again: Stop off, Start per selection
}

//--- compile-log navigation ---

void MainWindow::onCompileLogItemSelected(const CompileLogItemInfo& info) {
    //Only diagnostic lines carry a usable site (line/column 1-based).
    if (info.line == 0 || info.column == 0)
        return;
    locateSource(info.filePath, static_cast<int>(info.line),
                 static_cast<int>(info.column));
    m_ui->statusBar->showMessage(info.message);
}

void MainWindow::locateSource(const QString& filePath, int line,
                              int column) {
    editExistingFile(filePath);
    FileEditor* editor = m_editors.find(filePath);
    CodeEditor* code = editor != nullptr
        ? qobject_cast<CodeEditor*>(editor->widget()) : nullptr;
    if (code == nullptr)
        return;
    QTextCursor cursor(code->document());
    const QTextBlock block =
        code->document()->findBlockByNumber(line - 1);
    if (!block.isValid())
        return;
    cursor.setPosition(block.position() + column - 1);
    code->setTextCursor(cursor);
    code->ensureCursorVisible();
    code->setFocus();
}

} // namespace nlang
