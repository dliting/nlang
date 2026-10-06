/*--- MainWindowBuildRun.cpp - build and run pipeline of the NLang IDE
    main window: project/standalone builds, the embedded-terminal run
    lifecycle, compile-log navigation. ---*/
#include "MainWindow.h"
#include "CodeEditor.h"
#include "CompileLogBrowser.h"
#include "DebugClient.h"
#include "FileEditor.h"
#include "ProjectModel.h"
#include "SearchPathArgs.h"
#include "SettingsStore.h"
#include "SolutionTreeModel.h"

#include "terminal/TerminalWidget.h"

#include "ui_MainWindow.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QTextBlock>
#include <QTextCursor>

namespace nlang {

namespace {
//killPtySync's drain backstop: far above the real Kill-join-teardown
//(milliseconds), only there so a pathological waiter can't wedge the
//GUI thread forever.
constexpr int kKillDrainTimeoutMs = 3000;
} // namespace

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

bool MainWindow::saveProjectForBuild(ProjectNode& project) {
    //Save the dirty editors of this project's files so ncc sees the edits.
    for (const auto& file : project.files()) {
        FileEditor* editor = m_editors.find(file->absolutePath());
        if (editor != nullptr && editor->dirty() && !saveEditor(editor))
            return false;
    }
    return !project.isDirty() || saveProject(project);
}

bool MainWindow::prepareBuildOutput(ProjectNode* project,
                                    const QString& output) {
    m_ui->txtCompileOut->setProject(project);
    m_ui->txtCompileOut->clear();
    showOutputPage(m_ui->tabCompileOut);
    if (QDir().mkpath(QFileInfo(output).absolutePath()))
        return true;
    QMessageBox::warning(this, tr("Error"),
        tr("Cannot create the output directory '%1'.")
            .arg(QFileInfo(output).absolutePath()));
    return false;
}

bool MainWindow::runNccBuild(const QStringList& args,
                             const QString& workDir, QString* log) {
    //Synchronous build: ncc writes diagnostics on stderr, so merge the
    //channels before reading (plain readAll() would only see stdout).
    QProcess ncc(this);
    ncc.setProcessChannelMode(QProcess::MergedChannels);
    ncc.setWorkingDirectory(workDir);
    ncc.start(toolPath("ncc"), args);
    if (!ncc.waitForStarted(-1)) {
        *log = tr("Failed to start '%1'.").arg(toolPath("ncc"));
        return false;
    }
    ncc.waitForFinished(-1);
    //ncc emits UTF-8 bytes verbatim (source snippets inside diagnostics
    //carry the source encoding) — decode as UTF-8, not the local code
    //page, matching the debug page's wire decoding.
    *log = QString::fromUtf8(ncc.readAll());
    //Read the exit state only after a real run: start() resets it, so the
    //failed-start path would fake success sharing this expression.
    return ncc.exitStatus() == QProcess::NormalExit
        && ncc.exitCode() == 0;
}

bool MainWindow::buildProject(ProjectNode& project) {
    if (!saveProjectForBuild(project))
        return false;
    const QString output = outputFilePath(project);
    if (!prepareBuildOutput(&project, output))
        return false;
    //Project paths: ncc also reads them from the .nproj (-p), but passing
    //them here is harmless (de-duplicated); global paths exist only in the
    //IDE settings and must be supplied as -I.
    QStringList args = {"build", "-p", projectFilePath(&project),
                        "-o", output};
    args += buildImportArgs(projectImportPathList(project),
                            project.projectDir(),
                            SettingsStore::persisted().librarySearchPaths());
    //0.7.5: warning suppression is two-level -- the project's opt-in
    //adds on top of the global Tools > Options setting.
    if (project.noWarn() || SettingsStore::persisted().noWarn())
        args << "--no-warn";
    QString log;
    const bool succeeded = runNccBuild(
        args, project.projectDir(), &log);
    m_ui->txtCompileOut->append(log);
    m_ui->statusBar->showMessage(
        succeeded ? tr("Build succeeded") : tr("Build failed"));
    return succeeded;
}

void MainWindow::runProject(ProjectNode& project) {
    if (m_pty.IsRunning())
        return;
    const QString output = outputFilePath(project);
    if (!QFileInfo::exists(output)) {
        QMessageBox::warning(
            this, tr("Error"),
            tr("'%1' does not exist. Build the project first.").arg(output));
        return;
    }
    QStringList runArgs{output};
    //The project dir rides in the search list explicitly: the closure
    //loader resolves unlisted same-dir library units from it (the
    //compile side's base dir), independent of the process CWD.
    runArgs += appendImportArgs(projectSearchDirs(
        projectImportPathList(project), project.projectDir(),
        SettingsStore::persisted().librarySearchPaths()));
    beginTerminalRun(toolPath("nvm"), runArgs, project.projectDir());
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

    const QString output = standaloneNcuPath(filePath);
    if (!prepareBuildOutput(nullptr, output))
        return false;
    QStringList args = {"build", filePath, "-o", output};
    args += buildImportArgs({}, QString(),
        SettingsStore::persisted().librarySearchPaths());
    //Standalone files have no project half: only the global setting
    //decides whether --no-warn rides along.
    if (SettingsStore::persisted().noWarn())
        args << "--no-warn";
    QString log;
    const bool succeeded = runNccBuild(
        args, QFileInfo(filePath).absolutePath(), &log);
    m_ui->txtCompileOut->append(log);
    m_ui->statusBar->showMessage(
        succeeded ? tr("Build succeeded") : tr("Build failed"));
    return succeeded;
}

void MainWindow::runStandaloneFile(const QString& filePath) {
    if (m_pty.IsRunning())
        return;
    //A dirty editor must not run as the stale on-disk build.
    FileEditor* editor = m_editors.find(filePath);
    if (editor != nullptr && editor->dirty() && !saveEditor(editor))
        return;
    const QString output = standaloneNcuPath(filePath);
    //D2: unlike the project Run (which asks for a manual build first),
    //a missing/outdated module is rebuilt here automatically.
    if (!QFileInfo::exists(output) ||
        QFileInfo(output).lastModified() <
            QFileInfo(filePath).lastModified()) {
        if (!buildStandaloneFile(filePath))
            return;
    }
    //Run from the module's dir: an example writing files stays inside
    //the temp area (never the install dir); no example needs the
    //source dir as CWD (the e2e suite proves that).
    QStringList runArgs{output};
    //The source's dir mirrors ncc's compile-time base dir: the artifact
    //embeds no library code, so a same-dir library unit must resolve
    //from the run search path exactly like it resolved at compile time.
    runArgs += appendImportArgs(standaloneSearchDirs(
        filePath, SettingsStore::persisted().librarySearchPaths()));
    beginTerminalRun(toolPath("nvm"), runArgs,
                     QFileInfo(output).absolutePath());
}

void MainWindow::on_actStopRunning_triggered() {
    if (m_pty.IsRunning())
        m_pty.Kill();
}

void MainWindow::on_actClearBuild_triggered() {
    m_ui->txtCompileOut->clear();
    m_pTerminal->resetTerminal();
}

//--- embedded terminal lifecycle ---

bool MainWindow::beginTerminalRun(const QString& program,
                                  const QStringList& arguments,
                                  const QString& workingDirectory) {
    //Stale-state backstop: actStartRunning's enablement already keeps
    //Start idle while a child runs, but anything that still holds the
    //pty — including a natural exit whose Finished post is still queued
    //— must be torn down and drained synchronously here, or the old
    //child's tail output (or its onPtyFinished mode stomp) would leak
    //into the fresh session's screen.
    killPtySync();
    m_pTerminal->resetTerminal();
    //Character mode: keystrokes pass through to the child's console
    //(echo comes from the child's conhost, not from us).
    m_pTerminal->setMode(terminal::TerminalWidget::Mode::Character);
    showOutputPage(m_ui->tabExecuteOut);
    terminal::PtyProcess::StartError error =
        terminal::PtyProcess::StartError::None;
    if (!m_pty.Start(program, arguments, workingDirectory,
                     m_pTerminal->columns(), m_pTerminal->rows(), &error)) {
        m_pTerminal->feedUtf8(
            (error == terminal::PtyProcess::StartError::Unsupported
                 ? tr("Pseudo console is not supported on this system "
                      "(Windows 10 1809 or newer is required).")
                 : tr("Failed to start '%1'.").arg(program))
            + QLatin1Char('\n'));
        m_pTerminal->setMode(terminal::TerminalWidget::Mode::Idle);
        updateMenuState();
        return false;
    }
    m_ptyFinishPending = true;  //a Finished is owed until delivered
    updateMenuState();  // Running now: Start off, Stop on
    return true;
}

void MainWindow::killPtySync() {
    //Two reasons to drain, not one: a LIVE child (IsRunning), or a
    //finished one whose Finished post is still queued — the waiter
    //clears running BEFORE its post reaches the queue consumer, so
    //IsRunning alone leaves that window uncovered (m_ptyFinishPending
    //is the window-side witness).
    if (!m_pty.IsRunning() && !m_ptyFinishPending)
        return;
    if (m_pty.IsRunning()) {
        //Kill() joins the waiter thread, which emits Finished cross-
        //thread before that join returns — the post is made by here.
        m_pty.Kill();  //live child: terminate + teardown
    }
    //Pending-only: a natural exit with Finished queued — nothing to
    //kill. Calling Kill() here would race its killed=true into the
    //waiter's not-yet-evaluated verdict and relabel the exit as a
    //crash; leftover handles are collected by Start()/the destructor,
    //both of which call Kill().
    //
    //Delivery, not emission, is what we wait for — and the emission
    //has (almost) always happened before this point, so a connect-
    //based quit can never observe it. Poll the delivery witness in
    //sliced processEvents instead; the deadline stays the backstop
    //for a pathological waiter only. Leaving the post undelivered
    //would let onPtyFinished's setMode(Idle) land AFTER the next
    //phase (a debug session or a new run) has already set its own
    //mode, stomping it.
    QElapsedTimer budget;
    budget.start();
    while (m_ptyFinishPending && budget.elapsed() < kKillDrainTimeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void MainWindow::onPtyOutput(const QByteArray& bytes) {
    m_pTerminal->feedBytes(bytes);
}

void MainWindow::onPtyFinished(int exitCode, bool crashed) {
    m_ptyFinishPending = false;   //delivered: no drain is owed anymore
    m_pTerminal->feedUtf8(
        (crashed ? tr("The process crashed.")
                 : tr("Program exited with code %1.").arg(exitCode))
        + QLatin1Char('\n'));
    m_pTerminal->setMode(terminal::TerminalWidget::Mode::Idle);
    updateMenuState();  // Idle again: Stop off, Start per selection
}

void MainWindow::onTerminalLineCommitted(const QString& line) {
    //Line mode exists only for a debug session; a run child receives
    //its keystrokes through the pty directly, never through here.
    //Known limitation: the terminal's synthetic echo (spec §4) is
    //already on screen even when the client is in its Ended window and
    //silently drops this line — suppressing it would need the widget to
    //know delivery succeeded; accepted MVP trade-off.
    if (debugSessionLive())
        m_debugClient->sendStdin(line);
}

void MainWindow::onTerminalSizeChanged(int columns, int rows) {
    if (m_pty.IsRunning())
        m_pty.Resize(columns, rows);
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
