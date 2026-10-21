/*--- MainWindowDebug.cpp - debugging of the NLang IDE main window:
    the ndb session lifecycle and the debug views (breakpoint bookkeeping
    lives in MainWindowBreakpoints.cpp). ---*/
#include "MainWindow.h"
#include "BreakpointStore.h"
#include "CodeEditor.h"
#include "DebugClient.h"
#include "FileEditor.h"
#include "SearchPathArgs.h"
#include "SettingsStore.h"
#include "SolutionTreeModel.h"

#include "terminal/TerminalWidget.h"

#include "ui_MainWindow.h"

#include <QDir>
#include <QFileInfo>
#include <QTextCursor>
#include <QTreeWidgetItem>

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
    return standaloneNcuPath(standalone);
}

QStringList MainWindow::debugSearchPaths() const {
    //Both flows append the target's implicit base dir (the compile
    //side's own search root): the artifact embeds no library code, so a
    //same-dir library unit must resolve at load time like it resolved
    //at compile time — the debug session's CWD is the ARTIFACT dir, so
    //a redirected output would otherwise hide the project dir.
    const QStringList globalPaths =
        SettingsStore::persisted().librarySearchPaths();
    if (ProjectNode* project = currentProject())
        return projectSearchDirs(projectImportPathList(*project),
                                 project->projectDir(), globalPaths);
    const QString standalone = currentStandaloneTarget();
    if (standalone.isEmpty())
        return buildSearchDirs({}, QString(), globalPaths);
    return standaloneSearchDirs(standalone, globalPaths);
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
    //Run/debug mutual exclusion: the run child owns the terminal in
    //Character mode; a debug session needs it in Line mode, so a live
    //run is torn down synchronously first (the menu enablement already
    //steers the user away; this is the stale-state backstop).
    killPtySync();
    const QString modulePath = prepareDebugTarget();
    if (modulePath.isEmpty() || !QFileInfo::exists(modulePath))
        return false;

    createDebugClient();
    clearDebugViews();
    //Fresh screen; Line mode: Enter hands the edited line to the
    //debugged program through the machine channel (local echo and
    //editing, not a system console).
    m_pTerminal->resetTerminal();
    m_pTerminal->setMode(terminal::TerminalWidget::Mode::Line);
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
    m_selectedDebugContext = -1;
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
    connect(client, &DebugClient::contextReceived, this,
            &MainWindow::onDebugContextReceived);
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
                                int frameCount, int contextId) {
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
    //The server redirected inspection AND step target to the stopped
    //context; the page's selection marker follows the same instant.
    m_selectedDebugContext = contextId;
    const QString absolute = resolveDebugPath(file);
    locateSource(absolute, line, 1);   // same jump the compile log uses
    if (FileEditor* editor = m_editors.find(absolute)) {
        if (CodeEditor* code = qobject_cast<CodeEditor*>(editor->widget()))
            code->setStoppedLine(line);
    }
    clearDebugViews();
    m_debugClient->requestBacktrace();   // fills the stack tree
    m_debugClient->requestLocals(0);     // innermost frame's variables
    m_debugClient->requestContexts();    // fills the context tree
    setDebugStatus(tr("Paused: %1 (%2:%3) in context %4")
                       .arg(funcName,
                            QFileInfo(absolute).fileName())
                       .arg(line)
                       .arg(contextId));
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

void MainWindow::on_tvwDebugContexts_itemClicked(QTreeWidgetItem* item,
                                                 int column) {
    Q_UNUSED(column);
    if (item == nullptr || m_debugClient == nullptr
        || m_debugClient->state() != DebugClient::State::Stopped)
        return;
    //Selection = inspection AND step target (mainstream-IDE semantics):
    //the wire's `context <id>` re-addresses bt/frame/locals and arms the
    //context the next s/n/f steps. The selection member moves FIRST, so
    //context rows still arriving from an earlier request cannot pull the
    //marker back to the stopped context.
    m_selectedDebugContext = item->data(0, Qt::UserRole).toInt();
    m_debugClient->selectContext(m_selectedDebugContext);
    m_ui->tvwDebugStack->clear();
    m_ui->tvwDebugVars->clear();
    //The server-side frame selection reset with the context switch, so
    //the locals restart from frame 0; a frameless (finished) context
    //answers empty and the trees simply stay empty.
    m_debugClient->requestBacktrace();
    m_debugClient->requestLocals(0);
    const QString file = resolveDebugPath(
        item->data(0, Qt::UserRole + 1).toString());
    locateSource(file, item->data(0, Qt::UserRole + 2).toInt(), 1);
    setDebugStatus(tr("Context %1: %2 (%3:%4)")
                       .arg(m_selectedDebugContext)
                       .arg(item->data(0, Qt::UserRole + 3).toString(),
                            QFileInfo(file).fileName())
                       .arg(item->data(0, Qt::UserRole + 2).toInt()));
}

void MainWindow::onDebugLocalReceived(const QString& name,
                                      const QString& typeName,
                                      const QString& value) {
    auto* item = new QTreeWidgetItem(m_ui->tvwDebugVars);
    item->setText(0, name);
    item->setText(1, typeName);
    item->setText(2, value);
}

void MainWindow::onDebugContextReceived(int contextId, const QString& state,
                                        int heldByCarrier,
                                        const QString& funcName,
                                        const QString& file, int line) {
    auto* item = new QTreeWidgetItem(m_ui->tvwDebugContexts);
    item->setText(0, QString::number(contextId));
    //Carrier suffix mirrors ndb's threads row: who holds the execution
    //window (-1 = parked/queued/finished).
    QString location;
    if (!funcName.isEmpty())
        location = QStringLiteral("%1 (%2:%3)")
                       .arg(funcName, QFileInfo(file).fileName())
                       .arg(line);
    item->setText(1, heldByCarrier >= 0
                         ? QStringLiteral("%1 @%2").arg(state)
                               .arg(heldByCarrier)
                         : state);
    item->setText(2, location);   //frameless: stays empty
    //Click payload: the context id (selection), the location for the
    //editor jump, and the function for the status line.
    item->setData(0, Qt::UserRole, contextId);
    item->setData(0, Qt::UserRole + 1, file);
    item->setData(0, Qt::UserRole + 2, line);
    item->setData(0, Qt::UserRole + 3, funcName);
    //The single marker IS the tree's current item: the row matching the
    //selection (set by the stop or the user's click) carries it.
    if (contextId == m_selectedDebugContext)
        m_ui->tvwDebugContexts->setCurrentItem(item);
}

void MainWindow::onDebugOutput(const QString& text) {
    //Program output shares the run page's terminal; the wire splits
    //io.print into a text half plus a newline half, so insert verbatim.
    m_pTerminal->feedUtf8(text);
}

void MainWindow::onDebugError(const QString& report) {
    m_pTerminal->feedUtf8(report + QLatin1Char('\n'));
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
        m_pTerminal->feedUtf8(diagnostic + QLatin1Char('\n'));
    }
    endDebugSession();   // also clears the stop marker
    updateMenuState();
}

void MainWindow::onDebugCommandFailed(const QString& message) {
    //Per-command protocol failures are transient; surface them on the
    //terminal instead of a modal.
    m_pTerminal->feedUtf8(tr("ndb: %1").arg(message) + QLatin1Char('\n'));
}

void MainWindow::onDebugFailedToLaunch(const QString& error) {
    setDebugStatus(tr("Debug process exited abnormally"));
    m_pTerminal->feedUtf8(error + QLatin1Char('\n'));
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
    //Every session end converges here: the paused-line highlight and
    //the debug trees must not survive the session (exit, error, user
    //stop, window close).
    clearStoppedMarker();
    clearDebugViews();
    refreshBreakpointMarkers();   // the dots go hollow
    m_pTerminal->setMode(terminal::TerminalWidget::Mode::Idle);
    updateMenuState();
}

bool MainWindow::debugSessionLive() const {
    return m_debugClient != nullptr;
}

//--- debug view helpers ---

void MainWindow::clearStoppedMarker() {
    for (FileEditor* editor : m_editors.editors()) {
        if (CodeEditor* code = qobject_cast<CodeEditor*>(editor->widget()))
            code->setStoppedLine(0);
    }
}

void MainWindow::clearDebugViews() {
    m_ui->tvwDebugContexts->clear();
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

} // namespace nlang
