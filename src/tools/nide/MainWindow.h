/*--- MainWindow.h - main window of the NLang IDE ---*/
#ifndef NLANG_TOOLS_NIDE_MAIN_WINDOW_H
#define NLANG_TOOLS_NIDE_MAIN_WINDOW_H

#include <QPointer>
#include <QMainWindow>

#include "BreakpointStore.h"  // BreakpointStore is a value member
#include "FileEditor.h"  // EditorManager is a value member
#include "nlang/langservice/SymbolIndex.h"  // library symbol index
#include "RecentStore.h"  // RecentStore is a value member
#include "terminal/PtyProcess.h"  // PtyProcess is a value member

#include <QStringList>
#include <map>
#include <memory>
#include <utility>
#include <vector>

class QCloseEvent;
class QFileInfo;
class QIcon;
class QModelIndex;
class QTreeWidgetItem;

namespace nlang {

struct CompileLogItemInfo;
class CodeEditor;
class DebugClient;
class FileNode;
class HelpBrowser;
class ProjectNode;
class SolutionNode;
class SolutionTreeModel;

namespace terminal {
class TerminalWidget;
}

} // namespace nlang

namespace Ui {
class MainWindow;
}

//White-box IDE tests (defined in tests/test_nide/test_mainwindow.cpp).
class TestMainWindow;

namespace nlang {

//--- MainWindow: menus, editor tabs, solution tree, build & run.
//  Design notes:
//  - The .nsln solution is explicit: new/open/save/close solution
//    actions exist for it.
//  - BuildAll/Rebuild*/ClearAll/CancelBuild actions are omitted; the
//    debug menu is a real implementation on top of `ndb --machine`
//    (DebugClient): F5 debug sessions, breakpoints, stepping and the
//    debug output page.
//  - The output panes are direct tabOutput pages (not reparented
//    QDockWidgets).
//  - Build runs `ncc build -p <nproj> -o <npkg>` (a standalone .n
//    target builds to its .ncu), run launches `nvm` on that artifact.
//  - Unsaved-work prompts key on SolutionNode::hasUnsavedChanges (any
//    dirty project, or the solution's own state when first-class -- the
//    ephemeral wrapper created around an opened project never counts:
//    scaffolding is not user work; saving it promotes it).
//  - removeFile never prompts for a dirty editor of that file (the
//    editor stays open, so no edits are lost).
//  - addExistingFile does not open the added file in an editor.
//  - Non-diagnostic log lines emit no status-bar message.
//  - Files rename through one pipeline (renameFileEverywhere):
//    F2/tree-menu in-place edits and the tab-menu
//    dialog converge there; a dirty editor is saved to the old path
//    before the disk rename, and a domain rejection rolls the disk
//    rename back.
//  - File->New File joins a project in the tree: the selected
//    project, else the solution's sole project, else a standalone
//    editor.
//  - Splitter proportions persist across sessions via QSettings;
//    restore falls back to the editor-favoring
//    defaults on garbage or a collapsed pane.
class MainWindow : public QMainWindow {
    Q_OBJECT

    //White-box IDE tests read the pty members (the drain-latency pin
    //must observe the child's exit without event processing).
    friend class ::TestMainWindow;

public:
    explicit MainWindow(QWidget* parent = nullptr);
    //Out-of-line: the unique_ptr member deletes an incomplete Ui type
    //otherwise.
    ~MainWindow() override;

    //Layout persistence and docs-site page location are window-shell
    //helpers: free functions in WindowLayout.h / HelpPagePath.h (they
    //never touch MainWindow state, so tests drive them directly).

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    //--- 文件: solution lifecycle ---
    void on_actNewSolution_triggered();
    void on_actOpenSolution_triggered();
    void on_actSaveSolution_triggered();
    void on_actCloseSolution_triggered();

    //--- 文件: projects ---
    void on_actNewProject_triggered();
    void on_actOpenProject_triggered();
    void on_actSaveProject_triggered();
    void on_actCloseProject_triggered();

    //--- 文件: editors ---
    void on_actNewFile_triggered();
    void on_actOpenFile_triggered();
    void on_actCloseFile_triggered();
    void on_actSaveFile_triggered();
    void on_actSaveFileAs_triggered();
    void on_actSaveAll_triggered();
    void on_actQuit_triggered();

    //--- 项目 ---
    void on_actAddExistFile_triggered();
    void on_actAddNewFile_triggered();
    void on_actRemoveFile_triggered();
    void on_actProjectProp_triggered();

    //--- 编辑 ---
    void on_actGotoDefinition_triggered();

    //--- 构建 / 运行 / 调试 ---
    void on_actBuild_triggered();
    void on_actClearBuild_triggered();
    void on_actStartRunning_triggered();
    void on_actStopRunning_triggered();
    void on_actStartDebug_triggered();
    void on_actStopDebug_triggered();
    void on_actStepInto_triggered();
    void on_actStepOver_triggered();
    void on_actStepOut_triggered();
    void on_actToggleBreakpoint_triggered();
    void on_chkBreakOnThrow_toggled(bool checked);

    //--- 工具 ---
    void on_actToolsOptions_triggered();

    //--- 视图 / 帮助 ---
    void on_actViewSolution_triggered(bool checked);
    void on_actViewCodeEditor_triggered(bool checked);
    void on_actViewOutput_triggered(bool checked);
    void on_actViewToolBar_triggered(bool checked);
    void on_actHelpAbout_triggered();
    void on_actHelpGettingStarted_triggered();
    void on_actHelpLanguageSpec_triggered();
    void on_actHelpVmArch_triggered();
    void on_actHelpCliTools_triggered();

    //--- widgets ---
    void on_tabCodes_tabCloseRequested(int index);
    void on_tabCodes_currentChanged(int index);
    void on_tvwSolution_doubleClicked(const QModelIndex& index);
    void on_tvwSolution_customContextMenuRequested(const QPoint& pos);
    void on_dckSolution_visibilityChanged(bool visible);
    void on_tvwDebugStack_itemClicked(QTreeWidgetItem* item, int column);

    //--- embedded terminal (PtyProcess + TerminalWidget) ---
    void onPtyOutput(const QByteArray& bytes);
    void onPtyFinished(int exitCode, bool crashed);
    void onTerminalLineCommitted(const QString& line);
    void onTerminalSizeChanged(int columns, int rows);

    //--- non-widget signals (connected explicitly) ---
    void onEditorSaveStateChanged(FileEditor* editor);
    void onEditorPositionChanged();
    void onEditorFontZoom(int direction);  //notch: clamp+persist+apply
    //A code editor's cursor target flipped; mirrors the CURRENT editor
    //into the main menu's Go to Definition enablement.
    void onEditorJumpTargetChanged(bool available);
    void onSolutionSelectionChanged();
    void onFileRenameRequested(FileNode* file, const QString& newName);
    void onCompileLogItemSelected(const CompileLogItemInfo& info);
    void onBreakpointGutterClicked(int line);

    //--- debug session (DebugClient signals, connected per session) ---
    void onDebugBreakpointBound(int id, const QString& file, int line,
                                bool bound);
    void onDebugStopped(const QString& reason, int breakpointId,
                        const QString& funcName, const QString& file,
                        int line, int depth, int frameCount);
    void onDebugFrameReceived(int frameIndex, const QString& funcName,
                              const QString& file, int line);
    void onDebugLocalReceived(const QString& name, const QString& typeName,
                              const QString& value);
    void onDebugOutput(const QString& text);
    void onDebugError(const QString& report);
    void onDebugExited(int exitCode);
    void onDebugAbnormallyExited(const QString& diagnostic);
    void onDebugCommandFailed(const QString& message);
    void onDebugFailedToLaunch(const QString& error);

private:
    //--- recent list (File > Recent) ---
    //push + write-through persist (QSettings org/app from main.cpp).
    void noteRecent(const QString& absolutePath);
    void saveRecent();
    //Rebuild menuRecent from the store: existing-on-disk entries only,
    //same-name disambiguation, Clear at the end; the submenu hides
    //when nothing is clickable.
    void rebuildRecentMenu();
    //Dispatch a recent entry by extension (case-insensitive).
    void onRecentEntryTriggered();
    void onClearRecentTriggered();
    QIcon recentEntryIcon(const QFileInfo& info) const;

    //--- context accessors (null when nothing applicable is selected) ---
    FileEditor* currentEditor() const;
    //The current tab's CodeEditor (null for non-code tabs / no tab).
    CodeEditor* currentCodeEditor() const;
    ProjectNode* currentProject() const;
    FileNode* currentFile() const;
    //The project a File->New file joins: the tree selection, else the
    //solution's sole project; null when it stays standalone.
    ProjectNode* targetProjectForNewFile() const;

    //--- editors ---
    //New tab, focus, and the per-editor signal wiring.
    void addEditorTab(FileEditor* editor);
    //Apply a font size to every open code editor (Options / zoom path).
    void applyEditorFontPt(int pointSize);
    //Open (or focus) an existing file in an editor tab.
    void editExistingFile(const QString& filePath);
    //Go-to-definition target: open the symbol's source file at a line.
    void openLibraryDefinition(const QString& filePath, int line);
    //Create a new file on disk and edit it; false when creation fails
    //(the failure is shown to the user here).
    bool editNewFile(const QString& filePath);
    //Save with an error prompt on failure.
    bool saveEditor(FileEditor* editor);
    //Prompt on a dirty editor; false = keep it open.
    bool closeEditor(FileEditor* editor);
    //Remove the tab and delete the editor (after closeEditor agreed).
    void closeEditorTab(FileEditor* editor);
    //Drop every editor without prompts (closeEvent agreed to them);
    //blocks tab signals so the half-torn state emits nothing.
    void clearEditors();

    //--- context menus ---
    //The tab bar's context menu (Save / Save As... / Rename... /
    //Close / Close Others). Wired explicitly: the bar's object name is
    //not stable, so .ui auto-connect cannot reach it.
    void showTabContextMenu(const QPoint& pos);
    //Close every tab except keepIndex. Anchored on the keep WIDGET:
    //indexes shift as tabs close, so each round re-resolves them.
    void closeOtherEditorTabs(int keepIndex);

    //--- solution / projects ---
    //The .nproj location: the solution's stored reference for this
    //project (new projects store the path built by ProjectPropDialog).
    QString projectFilePath(const ProjectNode* project) const;
    //Save the .nproj with an error prompt on failure.
    bool saveProject(ProjectNode& project);
    //Save the .nsln (asking for the path when unnamed) plus every
    //dirty project (via saveWithProjects); false on failure or cancel.
    bool saveSolution();
    //Persist what the unsaved-work prompt guards: a first-class solution
    //goes out in one write (saveSolution); an ephemeral wrapper has no
    //file of its own, so only its dirty projects are written to their
    //own homes -- demanding a .nsln name here would resurrect the
    //phantom prompt the wrapper exemption removes.
    bool saveUnsavedChanges();
    //Prompt for unsaved work (SolutionNode::hasUnsavedChanges is the
    //authority); false vetoes the close.
    bool closeSolution();
    //No solution open -> create an ephemeral wrapper (never counts as
    //user work on its own; promoted on save).
    void ensureSolution();
    //Open a .nsln from a path. The unsaved-work gate (closeSolution)
    //lives INSIDE, not only in the menu handler: loadSolution replaces
    //an open model without any prompt. Pushes into the recent store on
    //success.
    bool openSolutionAtPath(const QString& path);
    //Open a .nproj from a path (ensureSolution first); pushes on
    //success. Null on failure (the error was shown here).
    ProjectNode* openProjectAtPath(const QString& path);
    //Select the project's tree row (after adding a project).
    void selectProject(ProjectNode* project);
    //Select the file's tree row (after adding a file).
    void selectFile(FileNode* file);

    //--- file rename pipeline ---
    //One rename everywhere: disk, domain node + tree mirror, and the
    //open editor. The order is fixed: validate the name, save dirty
    //edits to the OLD path, rename on disk, move the domain (rolling
    //the disk back if the domain rejects), then follow with the editor.
    //False leaves nothing moved. trackedFile may be null (a standalone
    //editor file outside the tree).
    //oldPath is BY VALUE on purpose: the domain rename rewrites the
    //FileNode's path member mid-call, and a reference parameter bound
    //to it would read the NEW path by the time the tail runs.
    bool renameFileEverywhere(QString oldPath,
                              const QString& newFileName,
                              FileNode* trackedFile);
    //Disk + domain half of the rename: persist the editor's dirty edits
    //to the OLD path, rename on disk, then move the domain node (a
    //domain rejection rolls the disk back so both layers stay
    //consistent). False leaves nothing moved.
    bool moveFileOnDisk(const QString& oldPath, const QString& newPath,
                        FileNode* trackedFile, FileEditor* editor);
    //The tree node for a file path; null when the file is standalone.
    FileNode* findFileNodeByPath(const QString& filePath) const;

    //--- standalone files (mirror of open untracked editors) ---
    //Absolute paths of the open editors no project tracks -- the tree's
    //standalone group is fed from this (nothing is persisted).
    QStringList standaloneEditorPaths() const;
    //Push standaloneEditorPaths() into the tree model and expand the
    //group's chain.
    void refreshStandaloneFiles();

    //--- build / run ---
    //Synchronous ncc build; false on failure (the log browser shows why).
    bool buildProject(ProjectNode& project);
    bool saveProjectForBuild(ProjectNode& project);
    bool prepareBuildOutput(ProjectNode* project, const QString& output);
    bool runNccBuild(const QStringList& args, const QString& workDir,
                     QString* log);
    void runProject(ProjectNode& project);
    //Standalone .n target: the selected standalone tree row, or -- when the
    //tree points at no project/standalone row -- the active editor tab if
    //it is an untracked file. A project target always wins over either.
    QString currentStandaloneTarget() const;
    //ncc build <file> -o <ncu> into the temp dir; false when ncc fails.
    bool buildStandaloneFile(const QString& filePath);
    //Auto-build first when the ncu is missing or older than the source,
    //then run it with nvm from the temp dir.
    void runStandaloneFile(const QString& filePath);
    //One launch path for both run entries: reset the terminal, switch
    //to Character mode and start the child on the pseudo console. False
    //reports the failure on the terminal itself (unsupported ConPTY /
    //spawn failure) and leaves the window idle.
    bool beginTerminalRun(const QString& program,
                          const QStringList& arguments,
                          const QString& workingDirectory);
    //Kill the live pty session and synchronously drain its Finished
    //signal (see the definition for why the drain must be in-line).
    void killPtySync();
    //The global build output directory when set, else
    //%TEMP%/nlang-nide/<stem>.ncu -- one slot per file stem.
    QString standaloneNcuPath(const QString& filePath) const;
    //Bring the output pane up (it may be toggled off) and switch to the
    //given page.
    void showOutputPage(QWidget* page);

    //--- debug ---
    //Resolve the current target, build it synchronously and launch a
    //fresh ndb session with every stored breakpoint preset. False keeps
    //the window idle (the compile log shows why).
    bool startDebugSession();
    QString prepareDebugTarget();
    QStringList debugSearchPaths() const;
    void sendDebugPrelude();
    //Create this session's DebugClient and wire its signals to the
    //onDebug* slots (one client per session: Ended is terminal there).
    void createDebugClient();
    //One convergence point for every session end: tear the client down
    //safely (deferred delete -- a signal may still be on the stack),
    //clear the stop marker and refresh the action states.
    void endDebugSession();
    //True while an ndb session is live (Launching/Running/Stopped).
    bool debugSessionLive() const;
    //One toggle path for F9 and gutter clicks: flips the stored table,
    //persists, repaints the gutters and mirrors into the live session.
    void toggleBreakpoint(CodeEditor* code, int line);
    //Mirrors a (path, line) toggle into the live session: immediate in
    //the command windows (Launching/Stopped), queued while Running and
    //flushed at the next stop.
    void sendBreakpointChange(const QString& filePath, int line, bool add);
    void flushPendingBreakpointChanges();
    //Retires at the next frozen window the ids whose `d` the wire
    //rejected while Running.
    void flushDeferredBreakpointRetires();
    //Push the table + session binding into every open editor's gutter.
    void refreshBreakpointMarkers();
    //Persist-through, like the recent list.
    void saveBreakpoints();
    //The stopped file field of the wire: absolute already for standalone
    //builds, project-relative for project builds -- anchor the latter at
    //the debug target's directory.
    QString resolveDebugPath(const QString& file) const;
    void setDebugStatus(const QString& text);
    void clearDebugViews();
    void clearStoppedMarker();

    //outputDir wins; else the global build output directory; else the
    //project directory + name + ".npkg" (the project package artifact).
    //The same path is passed to ncc -o, so build output and run target
    //agree.
    QString outputFilePath(const ProjectNode& project) const;
    //ncc.exe/nvm.exe/ndb.exe live next to nide.exe.
    QString toolPath(const QString& toolName) const;
    //Apply persisted toolbar icon size (32 or 48).
    void applyToolbarIconSize(const QString& size);

    //Rebuild the symbol index: stdlib + configured library dirs (global
    //settings + per-project import paths) + each open project's directory
    //+ every open editor's file directory; rebuilt at startup, after
    //search-path changes, and when the solution/project/editor set changes.
    void reindexConfiguredLibraries();
    void indexStdLib();
    void indexConfiguredDirs();
    void indexOpenEditorDirs();

    //Open filePath at line/column (1-based), opening an editor if needed.
    void locateSource(const QString& filePath, int line, int column);

    //Menu/toolbar enablement from the current tree/editor selection.
    void updateMenuState();
    //The File/Project menu share: enabled by the presence of an open
    //editor / project / file selection.
    void updateFileMenuState();
    //The debug-menu share of updateMenuState: F5/Stop/steps track the
    //session windows, the throw checkbox grays out while Running.
    //canBuild mirrors updateMenuState's target check.
    void updateDebugMenuState(bool canBuild);
    //Every signal wiring of the ctor (view, editors, tree, tabs, log,
    //run process) -- kept out of it to leave the setup sequence legible.
    void wireSignals();

    //--- help ---
    //Open the generated docs-site page in the embedded help browser
    //(a reused HelpBrowser window; see the member).
    void openHelpDocument(const QString& documentPagePath);

    //Declaration order matters: m_ui first, so it is destroyed LAST --
    //tabCodes must outlive the editors that clearEditors() tears down.
    std::unique_ptr<Ui::MainWindow> m_ui;
    SolutionTreeModel* m_solutionTree;
    EditorManager m_editors;
    langservice::SymbolIndex m_symbolIndex;  // indexed library symbols
    RecentStore m_recent;
    BreakpointStore m_breakpoints;
    QString m_solutionFilePath;  // empty = unsaved new solution
    terminal::PtyProcess m_pty;  // the program under Run (pseudo console)
    //True from a successful Start until onPtyFinished DELIVERS: the
    //waiter clears IsRunning() before its Finished post reaches the
    //event queue consumer, so this flag is the only window-side witness
    //that a Finished is still in flight (killPtySync drains it).
    bool m_ptyFinishPending = false;
    //The run page's viewport, created inside the terminalHost
    //container (.ui keeps only the plain QWidget placeholder).
    terminal::TerminalWidget* m_pTerminal = nullptr;

    //--- debug session state (all of it per-session) ---
    //One DebugClient per session: Ended is terminal on the client, so
    //every session creates a fresh one and endDebugSession() retires it.
    //Null whenever no session is live.
    std::unique_ptr<DebugClient> m_debugClient;
    bool m_debugStopRequested = false;  //stop was user-initiated
    QString m_debugBaseDir;  //anchors project-relative stopped files
    //Wire id per accepted breakpoint (the receipt's (file, line) -> id).
    std::map<std::pair<QString, int>, int> m_breakpointIds;
    //Toggles made while Running, replayed at the next frozen window.
    struct PendingBreakpointChange
    {
        QString filePath;
        int line = 0;
        bool add = false;
    };
    std::vector<PendingBreakpointChange> m_pendingBreakpointChanges;
    //Wire ids the receipt-site self-heal could not retire because the
    //session was Running (the wire takes `d` only in the command
    //windows); flushed at the next stop.
    std::vector<int> m_deferredBreakpointRetires;

    //The embedded help window. Null when closed (the browser deletes
    //itself on close); the pointer self-nulls then, so the next Help
    //menu entry creates a fresh one.
    QPointer<HelpBrowser> m_helpBrowser;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_MAIN_WINDOW_H
