/*--- MainWindowBreakpoints.cpp - breakpoint bookkeeping of the NLang IDE
    main window: the persisted store, gutter markers, and the live
    session's wire mirroring (queued toggles, receipt self-heal). ---*/
#include "MainWindow.h"
#include "BreakpointStore.h"
#include "CodeEditor.h"
#include "DebugClient.h"
#include "FileEditor.h"

#include "ui_MainWindow.h"

#include <QSettings>

#include <map>

namespace nlang {

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

} // namespace nlang
