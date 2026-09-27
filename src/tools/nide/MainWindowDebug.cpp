/*--- MainWindowDebug.cpp - debugging of the NLang IDE main window:
    ndb session lifecycle, breakpoint wiring and the debug views. ---*/
#include "MainWindow.h"
#include "BreakpointStore.h"
#include "CodeEditor.h"
#include "DebugClient.h"
#include "FileEditor.h"
#include "SearchPathArgs.h"
#include "SettingsStore.h"
#include "SolutionTreeModel.h"

#include "ui_MainWindow.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTextCursor>
#include <QTreeWidgetItem>

#include <map>
#include <utility>

namespace nlang {

//--- debug session ---

void MainWindow::on_actStartDebug_triggered() {
    //F5 dispatches by session state: start from idle, continue while
    //paused, ignore while launching/running (the action reflects this in
    //its enablement, this is the defensive mirror).
    if (m_debugClient != nullptr) {
        if (m_debugClient->state() == DebugClient::State::Stopped)
            m_debugClient->continueRun();
        updateMenuState();
        return;
    }
    startDebugSession();
}

QString MainWindow::prepareDebugTarget() {
    if (ProjectNode* project = currentProject()) {
        if (!buildProject(*project))
            return QString();
        m_debugBaseDir = project->projectDir();
        return outputFilePath(*project);
    }
    const QString standalone = currentStandaloneTarget();
    if (standalone.isEmpty())
        return QString();   // the action was disabled without a target
    if (!buildStandaloneFile(standalone))
        return QString();
    m_debugBaseDir = QFileInfo(standalone).absolutePath();
    return standaloneNmodPath(standalone);
}

QStringList MainWindow::debugSearchPaths() const {
    //Project paths first, then global (standalone sessions get global only).
    const QStringList globalPaths =
        SettingsStore::persisted().librarySearchPaths();
    if (ProjectNode* project = currentProject())
        return buildSearchDirs(projectImportPathList(*project),
                               project->projectDir(), globalPaths);
    return buildSearchDirs({}, QString(), globalPaths);
}

void MainWindow::sendDebugPrelude() {
    //Prelude: every stored breakpoint, then the throw toggle. `run` is
    //deferred by the client until hello AND every bp receipt arrived, so
    //issuing all three back to back has no handshake race.
    for (const QString& file : m_breakpoints.files())
        for (int line : m_breakpoints.linesOf(file))
            m_debugClient->addBreakpoint(file, line);
    m_debugClient->setBreakOnThrow(m_ui->chkBreakOnThrow->isChecked());
    m_debugClient->run();
}

bool MainWindow::startDebugSession() {
    const QString modulePath = prepareDebugTarget();
    if (modulePath.isEmpty() || !QFileInfo::exists(modulePath))
        return false;

    createDebugClient();
    clearDebugViews();
    m_ui->txtExecuteOut->clear();
    showOutputPage(m_ui->tabDebug);
    //Same CWD policy as Run: an example writing files stays inside its
    //module's directory.
    m_debugClient->setWorkingDirectory(
        QFileInfo(modulePath).absolutePath());
    if (!m_debugClient->launch(modulePath, debugSearchPaths())) {
        endDebugSession();
        return false;
    }
    sendDebugPrelude();
    setDebugStatus(tr("Debug started"));
    updateMenuState();
    return true;
}

void MainWindow::createDebugClient() {
    //One DebugClient per session: Ended is terminal there, so every
    //session creates a fresh instance (parented to the window, which
    //also gives tests a findChildren seam).
    m_debugClient = std::make_unique<DebugClient>(toolPath("ndb"), this);
    m_debugStopRequested = false;
    m_breakpointIds.clear();
    m_pendingBreakpointChanges.clear();
    m_deferredBreakpointRetires.clear();
    DebugClient* const client = m_debugClient.get();
    connect(client, &DebugClient::breakpointBound, this,
            &MainWindow::onDebugBreakpointBound);
    connect(client, &DebugClient::stopped, this,
            &MainWindow::onDebugStopped);
    connect(client, &DebugClient::frameReceived, this,
            &MainWindow::onDebugFrameReceived);
    connect(client, &DebugClient::localReceived, this,
            &MainWindow::onDebugLocalReceived);
    connect(client, &DebugClient::outputReceived, this,
            &MainWindow::onDebugOutput);
    connect(client, &DebugClient::errorReceived, this,
            &MainWindow::onDebugError);
    connect(client, &DebugClient::exited, this, &MainWindow::onDebugExited);
    connect(client, &DebugClient::abnormallyExited, this,
            &MainWindow::onDebugAbnormallyExited);
    connect(client, &DebugClient::commandFailed, this,
            &MainWindow::onDebugCommandFailed);
    connect(client, &DebugClient::failedToLaunch, this,
            &MainWindow::onDebugFailedToLaunch);
}

void MainWindow::on_actStopDebug_triggered() {
    if (m_debugClient == nullptr)
        return;
    m_debugStopRequested = true;
    //Unconditional kill (an infinite loop must stay terminable); the
    //abnormallyExited signal is the UI convergence point.
    m_debugClient->stop();
}

void MainWindow::on_actStepInto_triggered() {
    if (m_debugClient != nullptr && m_debugClient->stepInto())
        updateMenuState();
}

void MainWindow::on_actStepOver_triggered() {
    if (m_debugClient != nullptr && m_debugClient->stepOver())
        updateMenuState();
}

void MainWindow::on_actStepOut_triggered() {
    if (m_debugClient != nullptr && m_debugClient->stepOut())
        updateMenuState();
}

void MainWindow::on_chkBreakOnThrow_toggled(bool checked) {
    //The wire only accepts the toggle in the command windows; the
    //checkbox grays out while Running, and the next session start
    //re-sends the choice anyway.
    if (m_debugClient != nullptr
        && (m_debugClient->state() == DebugClient::State::Launching
            || m_debugClient->state() == DebugClient::State::Stopped))
        m_debugClient->setBreakOnThrow(checked);
}

void MainWindow::onDebugBreakpointBound(int id, const QString& file,
                                        int line, bool bound) {
    if (m_debugClient == nullptr)
        return;   //the session is already torn down
    if (bound && id > 0) {
        if (m_breakpoints.contains(file, line)) {
            m_breakpointIds[{BreakpointStore::normalizedKey(file), line}] = id;
        } else {
            //A receipt can lag its own undo (a Running-phase toggle
            //replays the add and the remove back to back at the next
            //stop, and the remove finds no wire id yet). Retire the
            //breakpoint ndb just materialized -- left alone it ghosts
            //(spurious stops) and the stale id duplicates on a re-toggle.
            //The wire takes `d` only in the command windows, and a
            //continue dispatched in the same event pass as the stop can
            //put the receipt in the Running window: defer that retire
            //to the next stop instead of dropping it.
            if (!m_debugClient->deleteBreakpoint(id))
                m_deferredBreakpointRetires.push_back(id);
        }
    }
    refreshBreakpointMarkers();   // the dot goes filled
}

void MainWindow::onDebugStopped(const QString& reason, int breakpointId,
                                const QString& funcName,
                                const QString& file, int line, int depth,
                                int frameCount) {
    Q_UNUSED(reason);
    Q_UNUSED(breakpointId);
    Q_UNUSED(depth);
    Q_UNUSED(frameCount);   // the stack tree fills from `bt` frames
    if (m_debugClient == nullptr)
        return;   //the session is already torn down
    showOutputPage(m_ui->tabDebug);
    flushDeferredBreakpointRetires();
    flushPendingBreakpointChanges();
    clearStoppedMarker();
    const QString absolute = resolveDebugPath(file);
    locateSource(absolute, line, 1);   // same jump the compile log uses
    if (FileEditor* editor = m_editors.find(absolute)) {
        if (CodeEditor* code = qobject_cast<CodeEditor*>(editor->widget()))
            code->setStoppedLine(line);
    }
    clearDebugViews();
    m_debugClient->requestBacktrace();   // fills the stack tree
    m_debugClient->requestLocals(0);     // innermost frame's variables
    setDebugStatus(tr("Paused: %1 (%2:%3)")
                       .arg(funcName,
                            QFileInfo(absolute).fileName())
                       .arg(line));
    updateMenuState();
}

void MainWindow::onDebugFrameReceived(int frameIndex,
                                      const QString& funcName,
                                      const QString& file, int line) {
    auto* item = new QTreeWidgetItem(m_ui->tvwDebugStack);
    item->setText(0, QString::number(frameIndex + 1));   // 1-based depth
    item->setText(1, funcName);
    item->setText(2, QFileInfo(file).fileName() + QLatin1Char(':')
                         + QString::number(line));
    //Click payload: the 0-based frame index (requestLocals) and the
    //frame's location for the jump.
    item->setData(0, Qt::UserRole, frameIndex);
    item->setData(0, Qt::UserRole + 1, file);
    item->setData(0, Qt::UserRole + 2, line);
}

void MainWindow::on_tvwDebugStack_itemClicked(QTreeWidgetItem* item,
                                              int column) {
    Q_UNUSED(column);
    if (item == nullptr || m_debugClient == nullptr
        || m_debugClient->state() != DebugClient::State::Stopped)
        return;
    m_ui->tvwDebugVars->clear();
    m_debugClient->requestLocals(item->data(0, Qt::UserRole).toInt());
    const QString file = resolveDebugPath(
        item->data(0, Qt::UserRole + 1).toString());
    locateSource(file, item->data(0, Qt::UserRole + 2).toInt(), 1);
}

void MainWindow::onDebugLocalReceived(const QString& name,
                                      const QString& typeName,
                                      const QString& value) {
    auto* item = new QTreeWidgetItem(m_ui->tvwDebugVars);
    item->setText(0, name);
    item->setText(1, typeName);
    item->setText(2, value);
}

void MainWindow::onDebugOutput(const QString& text) {
    //Program output shares the Run page; the wire splits io.print into
    //a text half plus a newline half, so insert verbatim.
    appendExecuteOutput(text);
}

void MainWindow::onDebugError(const QString& report) {
    appendExecuteOutput(report + QLatin1Char('\n'));
    setDebugStatus(tr("Runtime error"));
    endDebugSession();
    updateMenuState();
}

void MainWindow::onDebugExited(int exitCode) {
    setDebugStatus(tr("Exited (code %1)").arg(exitCode));
    endDebugSession();
    updateMenuState();
}

void MainWindow::onDebugAbnormallyExited(const QString& diagnostic) {
    //A user-initiated stop kills ndb, which surfaces HERE -- report it
    //as the normal end it was, not as a crash.
    if (m_debugStopRequested) {
        setDebugStatus(tr("Debug stopped"));
    } else {
        setDebugStatus(tr("Debug process exited abnormally"));
        appendExecuteOutput(diagnostic + QLatin1Char('\n'));
    }
    endDebugSession();   // also clears the stop marker
    updateMenuState();
}

void MainWindow::onDebugCommandFailed(const QString& message) {
    //Per-command protocol failures are transient; surface them in the
    //output page instead of a modal.
    appendExecuteOutput(tr("ndb: %1").arg(message) + QLatin1Char('\n'));
}

void MainWindow::onDebugFailedToLaunch(const QString& error) {
    setDebugStatus(tr("Debug process exited abnormally"));
    appendExecuteOutput(error + QLatin1Char('\n'));
    endDebugSession();
    updateMenuState();
}

void MainWindow::endDebugSession() {
    m_pendingBreakpointChanges.clear();
    m_deferredBreakpointRetires.clear();
    if (m_debugClient != nullptr) {
        //Deferred delete: this usually runs inside one of the client's
        //own signal handlers, so the object must outlive the emit.
        //release() hands the ownership to the event loop.
        //The client stays ALIVE until the deferred delete runs, and a
        //child line still buffered in the pipe can dispatch in that
        //window (closeEvent tears the session down while the process is
        //still running) -- cut the signal path before releasing, so no
        //late event reaches the slots with a released client.
        disconnect(m_debugClient.get(), nullptr, this, nullptr);
        m_debugClient->deleteLater();
        m_debugClient.release();
    }
    //Every session end converges here: the paused-line highlight must
    //not survive the session (exit, error, user stop, window close).
    clearStoppedMarker();
    refreshBreakpointMarkers();   // the dots go hollow
    updateMenuState();
}

bool MainWindow::debugSessionLive() const {
    return m_debugClient != nullptr;
}

//--- breakpoints ---

void MainWindow::on_actToggleBreakpoint_triggered() {
    FileEditor* editor = currentEditor();
    if (editor == nullptr)
        return;
    if (CodeEditor* code = qobject_cast<CodeEditor*>(editor->widget()))
        toggleBreakpoint(code, code->textCursor().blockNumber() + 1);
}

void MainWindow::onBreakpointGutterClicked(int line) {
    if (CodeEditor* code = qobject_cast<CodeEditor*>(sender()))
        toggleBreakpoint(code, line);
}

void MainWindow::toggleBreakpoint(CodeEditor* code, int line) {
    FileEditor* editor = m_editors.findEditor(code);
    if (editor == nullptr)
        return;
    const QString filePath = editor->filePath();
    const bool added = m_breakpoints.toggle(filePath, line);
    saveBreakpoints();
    refreshBreakpointMarkers();
    sendBreakpointChange(filePath, line, added);
}

void MainWindow::sendBreakpointChange(const QString& filePath, int line,
                                      bool add) {
    if (m_debugClient == nullptr)
        return;   // no session: the stored table is the whole truth
    if (m_debugClient->state() == DebugClient::State::Running) {
        //The wire has no command window while the program runs; replay
        //the toggle at the next frozen window.
        m_pendingBreakpointChanges.push_back({filePath, line, add});
        return;
    }
    if (add) {
        m_debugClient->addBreakpoint(filePath, line);
        return;
    }
    const auto it = m_breakpointIds.find(
        {BreakpointStore::normalizedKey(filePath), line});
    if (it != m_breakpointIds.end() && it->second > 0) {
        m_debugClient->deleteBreakpoint(it->second);
        m_breakpointIds.erase(it);
    }
}

void MainWindow::flushPendingBreakpointChanges() {
    //The stored table is the truth; the wire needs the NET effect per
    //line only. Coalescing to the last toggle keeps an F9 on/off/on
    //burst from replaying raw wire commands (a re-add next to an
    //already-materialized twin), and makes an add+undo pair send
    //nothing at all -- so no receipt can lag behind its own removal.
    std::map<std::pair<QString, int>, PendingBreakpointChange>
        lastChangeByLine;
    for (const PendingBreakpointChange& change : m_pendingBreakpointChanges)
        lastChangeByLine[{BreakpointStore::normalizedKey(change.filePath),
                          change.line}] = change;
    for (const auto& last : lastChangeByLine)
        sendBreakpointChange(last.second.filePath, last.second.line,
                             last.second.add);
    m_pendingBreakpointChanges.clear();
}

void MainWindow::flushDeferredBreakpointRetires() {
    for (int breakpointId : m_deferredBreakpointRetires)
        m_debugClient->deleteBreakpoint(breakpointId);
    m_deferredBreakpointRetires.clear();
}

void MainWindow::refreshBreakpointMarkers() {
    for (FileEditor* editor : m_editors.editors()) {
        CodeEditor* code = qobject_cast<CodeEditor*>(editor->widget());
        if (code == nullptr)
            continue;
        QSet<int> lines = m_breakpoints.linesOf(editor->filePath());
        QSet<int> boundLines;
        if (m_debugClient != nullptr) {
            const QString key =
                BreakpointStore::normalizedKey(editor->filePath());
            for (const auto& bound : m_breakpointIds)
                if (bound.first.first == key)
                    boundLines.insert(bound.first.second);
        }
        code->setBreakpointLines(lines);
        code->setBoundBreakpointLines(boundLines);
    }
}

void MainWindow::saveBreakpoints() {
    QSettings settings;
    m_breakpoints.save(settings);
}

//--- debug view helpers ---

void MainWindow::clearStoppedMarker() {
    for (FileEditor* editor : m_editors.editors()) {
        if (CodeEditor* code = qobject_cast<CodeEditor*>(editor->widget()))
            code->setStoppedLine(0);
    }
}

void MainWindow::clearDebugViews() {
    m_ui->tvwDebugStack->clear();
    m_ui->tvwDebugVars->clear();
}

void MainWindow::setDebugStatus(const QString& text) {
    m_ui->lblDebugStatus->setText(text);
}

QString MainWindow::resolveDebugPath(const QString& file) const {
    //Standalone builds record absolute source paths; project builds
    //record them .nproj-relative -- anchor those at the target's dir.
    if (file.isEmpty() || QFileInfo(file).isAbsolute())
        return file;
    return QDir(m_debugBaseDir).filePath(file);
}

void MainWindow::appendExecuteOutput(const QString& text) {
    QTextCursor cursor = m_ui->txtExecuteOut->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    m_ui->txtExecuteOut->setTextCursor(cursor);
}

} // namespace nlang
