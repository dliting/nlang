/*--- MainWindow.h - main window of the NLang IDE ---*/
#ifndef NLANG_TOOLS_NIDE_MAIN_WINDOW_H
#define NLANG_TOOLS_NIDE_MAIN_WINDOW_H

#include <QPointer>
#include <QProcess>
#include <QMainWindow>

#include "FileEditor.h"  // EditorManager is a value member
#include "RecentStore.h"  // RecentStore is a value member

#include <QStringList>
#include <memory>

class QCloseEvent;
class QFileInfo;
class QIcon;
class QModelIndex;
class QSettings;

namespace nlang {

struct CompileLogItemInfo;
class FileNode;
class HelpBrowser;
class ProjectNode;
class SolutionNode;
class SolutionTreeModel;

} // namespace nlang

namespace Ui {
class MainWindow;
}

namespace nlang {

//--- MainWindow: menus, editor tabs, solution tree, build & run.
//  Ported from EN's MainWindow with these deviations:
//  - The .nsln solution is explicit (EN always had an implicit in-memory
//    one): new/open/save/close solution actions were added.
//  - Dead EN menu actions (BuildAll/Rebuild*/ClearAll/CancelBuild and
//    the whole debug family -- EN never implemented them) are dropped.
//  - The output panes are direct tabOutput pages (EN reparented two
//    QDockWidgets into the tab widget from the constructor).
//  - Build runs `ncc build -p <nproj> -o <nmod>`, run launches
//    `nvm <nmod>` (EN: `ncc -f <nprj>` / `nloader <npkg>`).
//  - Unsaved-work prompts key on solution-level state (the solution is
//    dirty after add/remove), never on per-project isDirty() alone: a
//    project created with all-default properties is not project-dirty.
//  - removeFile never prompts for a dirty editor of that file (the
//    editor stays open, so no edits are lost).
//  - addExistingFile does not open the added file in an editor.
//  - Non-diagnostic log lines emit no status-bar message (EN showed
//    every clicked line's text).
//  - Files rename through one pipeline (renameFileEverywhere) with no
//    EN counterpart: F2/tree-menu in-place edits and the tab-menu
//    dialog converge there; a dirty editor is saved to the old path
//    before the disk rename, and a domain rejection rolls the disk
//    rename back.
//  - File->New File joins a project in the tree (EN's new-file menu
//    never touched a project): the selected project, else the
//    solution's sole project, else a standalone editor.
//  - Splitter proportions persist across sessions via QSettings (EN
//    stored nothing); restore falls back to the editor-favoring
//    defaults on garbage or a collapsed pane.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    //Out-of-line: the unique_ptr member deletes an incomplete Ui type
    //otherwise.
    ~MainWindow() override;

    //--- layout (default proportions + QSettings persistence) ---
    //Splitter proportions favoring the code editor: narrow solution
    //column, editor-dominant right side. Static on purpose: restore
    //logic and tests apply it to any window.
    static void applyDefaultLayout(MainWindow& window);

    //Persist/restore both splitter states (QMainWindow::saveState does
    //NOT cover central-widget splitters). Tests inject a temporary ini;
    //restore falls back to the default layout and returns false on
    //garbage or a collapsed pane.
    static void saveLayout(const MainWindow& window, QSettings& settings);
    static bool restoreLayout(MainWindow& window, QSettings& settings);

    //--- help (docs site in the embedded viewer) ---
    //First ancestor dir whose docs/site holds the page; empty when
    //none (installed layout: bin/../docs/site one hop up; dev tree
    //resolves a few hops deeper). documentPagePath is relative to
    //docs/site without the .html suffix. Tests call it directly.
    static QString locateHelpPage(const QString& documentPagePath);

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

    //--- 构建 / 运行 ---
    void on_actBuild_triggered();
    void on_actClearBuild_triggered();
    void on_actStartRunning_triggered();
    void on_actStopRunning_triggered();

    //--- 视图 / 帮助 ---
    void on_actViewSolution_triggered(bool checked);
    void on_actViewCodeEditor_triggered(bool checked);
    void on_actViewOutput_triggered(bool checked);
    void on_actViewToolBar_triggered(bool checked);
    void on_actHelpAbout_triggered();
    void on_actHelpGettingStarted_triggered();
    void on_actHelpLanguageSpec_triggered();
    void on_actHelpVmArch_triggered();

    //--- widgets ---
    void on_tabCodes_tabCloseRequested(int index);
    void on_tabCodes_currentChanged(int index);
    void on_tvwSolution_doubleClicked(const QModelIndex& index);
    void on_tvwSolution_customContextMenuRequested(const QPoint& pos);
    void on_dckSolution_visibilityChanged(bool visible);

    //--- non-widget signals (connected explicitly) ---
    void onEditorSaveStateChanged(FileEditor* editor);
    void onEditorPositionChanged();
    void onSolutionSelectionChanged();
    void onFileRenameRequested(FileNode* file, const QString& newName);
    void onCompileLogItemSelected(const CompileLogItemInfo& info);
    void onExecOutput();
    void onExecFinished(int exitCode, QProcess::ExitStatus exitStatus);

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
    ProjectNode* currentProject() const;
    FileNode* currentFile() const;
    //The project a File->New file joins: the tree selection, else the
    //solution's sole project; null when it stays standalone.
    ProjectNode* targetProjectForNewFile() const;

    //--- editors ---
    //New tab, focus, and the per-editor signal wiring.
    void addEditorTab(FileEditor* editor);
    //Open (or focus) an existing file in an editor tab.
    void editExistingFile(const QString& filePath);
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
    //Any unsaved work in the solution graph?
    bool isSolutionModified() const;
    //Prompt for unsaved work; false vetoes the close.
    bool closeSolution();
    //No solution open -> create an empty one (EN always had a solution).
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
    bool renameFileEverywhere(const QString& oldPath,
                              const QString& newFileName,
                              FileNode* trackedFile);
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
    void buildProject(ProjectNode& project);
    void runProject(ProjectNode& project);
    //Standalone .n target: the selected standalone tree row, or -- when the
    //tree points at no project/standalone row -- the active editor tab if
    //it is an untracked file. A project target always wins over either.
    QString currentStandaloneTarget() const;
    //ncc build <file> -o <nmod> into the temp dir; false when ncc fails.
    bool buildStandaloneFile(const QString& filePath);
    //Auto-build first when the nmod is missing or older than the source,
    //then run it with nvm from the temp dir.
    void runStandaloneFile(const QString& filePath);
    //%TEMP%/nlang-nide/<stem>.nmod -- one slot per file stem.
    QString standaloneNmodPath(const QString& filePath) const;
    //Bring the output pane up (it may be toggled off) and switch to the
    //given page.
    void showOutputPage(QWidget* page);

    //outputDir ("" = the project directory) + name + ".nmod"; the same
    //path is passed to ncc -o, so build output and run target agree.
    QString outputFilePath(const ProjectNode& project) const;
    //ncc.exe/nvm.exe live next to nide.exe.
    QString toolPath(const QString& toolName) const;

    //Open filePath at line/column (1-based), opening an editor if needed.
    void locateSource(const QString& filePath, int line, int column);

    //Menu/toolbar enablement from the current tree/editor selection.
    void updateMenuState();

    //--- help ---
    //Open the generated docs-site page in the embedded help browser
    //(a reused HelpBrowser window; see the member).
    void openHelpDocument(const QString& documentPagePath);

    //Declaration order matters: m_ui first, so it is destroyed LAST --
    //tabCodes must outlive the editors that clearEditors() tears down.
    std::unique_ptr<Ui::MainWindow> m_ui;
    SolutionTreeModel* m_solutionTree;
    EditorManager m_editors;
    RecentStore m_recent;
    QString m_solutionFilePath;  // empty = unsaved new solution
    QProcess m_executed;         // the program under Run (nvm child)
    //The embedded help window. Null when closed (the browser deletes
    //itself on close); the pointer self-nulls then, so the next Help
    //menu entry creates a fresh one.
    QPointer<HelpBrowser> m_helpBrowser;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_MAIN_WINDOW_H
