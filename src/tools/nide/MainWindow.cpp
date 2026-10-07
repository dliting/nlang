/*--- MainWindow.cpp - main window of the NLang IDE: construction,
    layout persistence, context accessors, menu state and tool paths.
    The domain TU family: MainWindowSolution.cpp (solution/project),
    MainWindowEditors.cpp (files/editors/rename), MainWindowBuildRun.cpp,
    MainWindowDebug.cpp, MainWindowHelp.cpp (tools/view/help/recent). ---*/
#include "MainWindow.h"
#include "BreakpointStore.h"
#include "CompileLogBrowser.h"
#include "DebugClient.h"
#include "FileEditor.h"
#include "ProjectModel.h"
#include "RecentStore.h"
#include "SearchPathArgs.h"
#include "SettingsStore.h"
#include "SolutionTreeModel.h"
#include "terminal/TerminalWidget.h"

#include "ui_MainWindow.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QMenu>
#include <QSettings>
#include <QSet>
#include <QSplitter>
#include <QTabBar>
#include <QVBoxLayout>

namespace nlang {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_ui(new Ui::MainWindow)
    , m_solutionTree(new SolutionTreeModel(this))
    , m_editors(this)
{
    m_ui->setupUi(this);
    //The run page's viewport: the .ui keeps only the terminalHost
    //placeholder, the terminal itself is a code-created child (it must
    //exist before wireSignals connects to it).
    QVBoxLayout* terminalLayout = new QVBoxLayout(m_ui->terminalHost);
    terminalLayout->setContentsMargins(0, 0, 0, 0);
    m_pTerminal = new terminal::TerminalWidget(m_ui->terminalHost);
    terminalLayout->addWidget(m_pTerminal);
    //Terminal replies (keystrokes encoded by the emulator) go straight
    //to the pty child. The sink captures `this` raw: the widget is a
    //direct child destroyed with the window, and feedBytes is only ever
    //invoked from the GUI thread (PtyProcess delivers via queued
    //signals).
    m_pTerminal->setByteSink([this](const std::string& bytes) {
        m_pty.Write(QByteArray(bytes.data(), int(bytes.size())));
    });
    wireSignals();

    m_ui->statusBar->showMessage(tr("Ready"));
    //A saved layout wins; the first run (or unreadable state) gets the
    //editor-favoring default proportions.
    QSettings settings;
    if (!restoreLayout(*this, settings))
        applyDefaultLayout(*this);
    m_recent.load(settings);
    m_breakpoints.load(settings);
    //The FILE menu's aboutToShow drives the rebuild: an empty submenu
    //entry must be hidden before the menu shows (menuRecent's own
    //signal would fire too late, entry already visible).
    connect(m_ui->menuFile, &QMenu::aboutToShow,
            this, &MainWindow::rebuildRecentMenu);
    //QMenu hides item tooltips by default; the recent list relies on the
    //full-path tooltip to disambiguate same-name entries (spec §6).
    m_ui->menuRecent->setToolTipsVisible(true);
    //Index stdlib + every configured library dir (global + projects) for
    //signature help / completion / F12. A missing stdlib is non-fatal:
    //code assistance stays dormant. Re-run after the search paths change.
    reindexConfiguredLibraries();
    updateMenuState();
    applyToolbarIconSize(SettingsStore::persisted().toolbarIconSize());
}

void MainWindow::wireSignals() {
    m_ui->tvwSolution->setModel(m_solutionTree);
    connect(m_ui->tvwSolution->selectionModel(),
            &QItemSelectionModel::currentRowChanged, this,
            &MainWindow::onSolutionSelectionChanged);
    connect(&m_editors, &EditorManager::saveStateChanged, this,
            &MainWindow::onEditorSaveStateChanged);
    connect(m_solutionTree, &SolutionTreeModel::fileRenameRequested, this,
            &MainWindow::onFileRenameRequested);
    //The tab bar needs its own policy: the tab widget's does not reach
    //it. Auto-connect cannot wire this (the bar's object name is not
    //stable across .ui generation).
    m_ui->tabCodes->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_ui->tabCodes->tabBar(), &QTabBar::customContextMenuRequested,
            this, &MainWindow::showTabContextMenu);
    connect(m_ui->txtCompileOut, &CompileLogBrowser::lineSelected, this,
            &MainWindow::onCompileLogItemSelected);

    //Embedded terminal: pty output feeds the emulator (queued from the
    //reader thread), Finished drives the exit line + menu state, the
    //widget's committed lines reach the debug session's stdin channel,
    //and grid relayouts resize the live pseudo console.
    connect(&m_pty, &terminal::PtyProcess::OutputReady, this,
            &MainWindow::onPtyOutput);
    connect(&m_pty, &terminal::PtyProcess::Finished, this,
            &MainWindow::onPtyFinished);
    connect(m_pTerminal, &terminal::TerminalWidget::lineCommitted, this,
            &MainWindow::onTerminalLineCommitted);
    connect(m_pTerminal, &terminal::TerminalWidget::sizeChanged, this,
            &MainWindow::onTerminalSizeChanged);
}

void MainWindow::applyToolbarIconSize(const QString& size) {
    const int px = (size == TOOLBAR_ICON_LARGE) ? 48 : 32;
    m_ui->mainToolBar->setIconSize(QSize(px, px));
}

MainWindow::~MainWindow() {
    //Same teardown as closeEvent (whose prompts already ran or don't
    //apply): drop the editors before the members destroy themselves.
    clearEditors();
}

namespace {

//Default splitter proportions. QSplitter::setSizes reads them as
//RELATIVE shares, so these are not pixels: the solution column keeps a
//narrow fifth, the output pane a quarter of the vertical space.
const int DEFAULT_SOLUTION_TREE_SHARE = 20;
const int DEFAULT_EDITOR_SHARE = 80;
const int DEFAULT_CODE_SHARE = 75;
const int DEFAULT_OUTPUT_SHARE = 25;

//QSettings keys for the two splitter states.
const char* const LAYOUT_SOLUTION_SPLITTER_KEY =
    "layout/solutionSplitter";
const char* const LAYOUT_EDITOR_SPLITTER_KEY = "layout/editorSplitter";

} // namespace

//--- layout ---

void MainWindow::applyDefaultLayout(MainWindow& window) {
    //The tree column must not grab horizontal space (stretch 0/1);
    //inside the right column the editor pane wins (stretch 1/0).
    QSplitter* solutionSplitter =
        window.findChild<QSplitter*>(QStringLiteral("splitter"));
    if (solutionSplitter != nullptr) {
        solutionSplitter->setStretchFactor(0, 0);
        solutionSplitter->setStretchFactor(1, 1);
        solutionSplitter->setSizes({DEFAULT_SOLUTION_TREE_SHARE,
                                     DEFAULT_EDITOR_SHARE});
    }
    QSplitter* editorSplitter =
        window.findChild<QSplitter*>(QStringLiteral("splitter_2"));
    if (editorSplitter != nullptr) {
        editorSplitter->setStretchFactor(0, 1);
        editorSplitter->setStretchFactor(1, 0);
        editorSplitter->setSizes({DEFAULT_CODE_SHARE,
                                  DEFAULT_OUTPUT_SHARE});
    }
}

void MainWindow::saveLayout(const MainWindow& window,
                            QSettings& settings) {
    if (QSplitter* splitter =
            window.findChild<QSplitter*>(QStringLiteral("splitter")))
        settings.setValue(LAYOUT_SOLUTION_SPLITTER_KEY,
                          splitter->saveState());
    if (QSplitter* splitter =
            window.findChild<QSplitter*>(QStringLiteral("splitter_2")))
        settings.setValue(LAYOUT_EDITOR_SPLITTER_KEY,
                          splitter->saveState());
}

bool MainWindow::restoreLayout(MainWindow& window, QSettings& settings) {
    QSplitter* solutionSplitter =
        window.findChild<QSplitter*>(QStringLiteral("splitter"));
    QSplitter* editorSplitter =
        window.findChild<QSplitter*>(QStringLiteral("splitter_2"));
    if (solutionSplitter == nullptr || editorSplitter == nullptr)
        return false;

    const bool restored =
        solutionSplitter->restoreState(
            settings.value(LAYOUT_SOLUTION_SPLITTER_KEY).toByteArray())
        && editorSplitter->restoreState(
            settings.value(LAYOUT_EDITOR_SPLITTER_KEY).toByteArray());
    //A state with a collapsed pane is as useless as no state at all.
    const bool panesVisible =
        solutionSplitter->sizes().at(0) > 0
        && solutionSplitter->sizes().at(1) > 0
        && editorSplitter->sizes().at(0) > 0
        && editorSplitter->sizes().at(1) > 0;
    if (!restored || !panesVisible) {
        applyDefaultLayout(window);
        return false;
    }
    return true;
}

//--- context accessors ---

FileEditor* MainWindow::currentEditor() const {
    QWidget* widget = m_ui->tabCodes->currentWidget();
    return widget != nullptr ? m_editors.findEditor(widget) : nullptr;
}

ProjectNode* MainWindow::currentProject() const {
    SolutionTreeItem* item =
        m_solutionTree->itemAt(m_ui->tvwSolution->currentIndex());
    if (item == nullptr)
        return nullptr;
    if (item->nodeType() == SolutionTreeItem::NT_File)
        return item->file()->project();
    if (item->nodeType() == SolutionTreeItem::NT_Project)
        return item->project();
    return nullptr;
}

FileNode* MainWindow::currentFile() const {
    SolutionTreeItem* item =
        m_solutionTree->itemAt(m_ui->tvwSolution->currentIndex());
    return item != nullptr && item->nodeType() == SolutionTreeItem::NT_File
        ? item->file() : nullptr;
}

//The project a File->New file joins: the tree selection first, then the
//solution's sole project (one project needs no choosing); without a
//solution (or with several unselected) the file stays standalone.
ProjectNode* MainWindow::targetProjectForNewFile() const {
    ProjectNode* project = currentProject();
    if (project != nullptr)
        return project;
    SolutionNode* solution = m_solutionTree->solutionNode();
    if (solution != nullptr && solution->projectCount() == 1)
        return solution->projects().front().get();
    return nullptr;
}

//--- output / tool locations ---

void MainWindow::showOutputPage(QWidget* page) {
    //setChecked alone doesn't emit triggered(), so the pane is shown
    //here explicitly; the checked state keeps the view menu in sync.
    m_ui->actViewOutput->setChecked(true);
    m_ui->tabOutput->setVisible(true);
    m_ui->tabOutput->setCurrentWidget(page);
}

QString MainWindow::outputFilePath(const ProjectNode& project) const {
    //Explicit .nproj outputDir wins; the global build output directory
    //(Tools > Options) applies when unset; the project directory
    //remains the default (the .nproj default). The artifact is the
    //.npkg package (phase 6).
    return SettingsStore::resolveProjectPackagePath(
        project.outputDir(), project.projectDir(),
        SettingsStore::persisted().buildOutputDir(), project.name());
}

QString MainWindow::standaloneNcuPath(const QString& filePath) const {
    //The global build output directory redirects the per-stem slot;
    //unset keeps the per-user temp area (the examples dir may be
    //read-only in the installed layout, and we never write next to
    //the source).
    const QString path = SettingsStore::resolveStandaloneNcuPath(
        SettingsStore::persisted().buildOutputDir(), filePath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    return path;
}

QString MainWindow::toolPath(const QString& toolName) const {
    //ncc/nvm sit next to nide.exe (a CMake POST_BUILD step arranges
    //that for developer builds; deployments keep them together).
    return QDir(QCoreApplication::applicationDirPath()).filePath(toolName);
}

//--- close ---

void MainWindow::closeEvent(QCloseEvent* event) {
    //Kill the debug child first: no prompt must race a live session, and
    //the teardown must not fire signal handlers into the closing window.
    if (m_debugClient != nullptr) {
        m_debugStopRequested = true;
        m_debugClient->stop();
        endDebugSession();
    }
    //The pty run child symmetrically: killed and drained HERE, while the
    //window is still alive to deliver Finished — the destructor's Kill()
    //would terminate the child but can no longer run the drain.
    killPtySync();
    if (!closeSolution()) {
        event->ignore();
        return;
    }
    for (FileEditor* editor : m_editors.editors()) {
        if (!closeEditor(editor)) {
            event->ignore();
            return;
        }
    }
    clearEditors();
    //Only an accepted close persists the layout -- a vetoed one keeps
    //the previous state.
    QSettings settings;
    saveLayout(*this, settings);
    event->accept();
}

//--- menu state ---

//The File/Project menu share of updateMenuState: every action there
//turns on the presence of an open editor / project / file selection.
void MainWindow::updateFileMenuState() {
    const bool hasEditor = currentEditor() != nullptr;
    const bool hasProject = currentProject() != nullptr;

    m_ui->actNewFile->setEnabled(true);
    m_ui->actOpenFile->setEnabled(true);
    m_ui->actSaveFile->setEnabled(hasEditor);
    m_ui->actSaveFileAs->setEnabled(hasEditor);
    m_ui->actCloseFile->setEnabled(hasEditor);

    m_ui->actNewProject->setEnabled(true);
    m_ui->actOpenProject->setEnabled(true);
    m_ui->actSaveProject->setEnabled(hasProject);
    m_ui->actCloseProject->setEnabled(hasProject);
    m_ui->actAddExistFile->setEnabled(hasProject);
    m_ui->actAddNewFile->setEnabled(hasProject);
    m_ui->actRemoveFile->setEnabled(currentFile() != nullptr);
    m_ui->actProjectProp->setEnabled(hasProject);
}

void MainWindow::updateMenuState() {
    //First line on purpose: the group must mirror the editors BEFORE
    //the enablement reads below consult the (possibly just-mutated)
    //tree selection. Every editor/solution mutation converges here.
    refreshStandaloneFiles();
    updateFileMenuState();

    //A project OR a standalone .n target can be built. While a debug
    //session is live, Build/Run stay off: a mid-session rebuild would
    //rewrite the very artifact the debugger is executing (bytecode
    //offsets shift under the session) and a Run child would interleave
    //its output on the shared run page.
    const bool hasStandaloneTarget = !currentStandaloneTarget().isEmpty();
    const bool canBuild = currentProject() != nullptr || hasStandaloneTarget;
    const bool debugLive = debugSessionLive();
    m_ui->actBuild->setEnabled(canBuild && !debugLive);

    //Run lifecycle: Start needs a build target AND an idle process; Stop
    //is live exactly while the pty child runs.
    const bool running = m_pty.IsRunning();
    m_ui->actStartRunning->setEnabled(canBuild && !running && !debugLive);
    m_ui->actStopRunning->setEnabled(running);

    updateDebugMenuState(canBuild);

    const bool hasSolution = m_solutionTree->hasSolution();
    m_ui->actSaveSolution->setEnabled(hasSolution);
    m_ui->actCloseSolution->setEnabled(hasSolution);
    m_ui->actNewSolution->setEnabled(true);
    m_ui->actOpenSolution->setEnabled(true);
}

void MainWindow::updateDebugMenuState(bool canBuild) {
    //Debug lifecycle: F5 doubles as Continue while paused; Stop and the
    //steps track the session windows; the checkbox grays out while the
    //program runs (the wire accepts the toggle only in the command
    //windows, Launching/Stopped).
    const bool debugLive = debugSessionLive();
    const bool debugStopped = debugLive
        && m_debugClient->state() == DebugClient::State::Stopped;
    m_ui->actStartDebug->setEnabled(
        (canBuild && !debugLive) || debugStopped);
    m_ui->actStopDebug->setEnabled(debugLive);
    m_ui->actStepInto->setEnabled(debugStopped);
    m_ui->actStepOver->setEnabled(debugStopped);
    m_ui->actStepOut->setEnabled(debugStopped);
    m_ui->actToggleBreakpoint->setEnabled(currentEditor() != nullptr);
    m_ui->chkBreakOnThrow->setEnabled(
        !debugLive || m_debugClient->state() != DebugClient::State::Running);
}

//--- library symbol indexing ---

void MainWindow::reindexConfiguredLibraries() {
    //Clear drops the loaded-dir markers too, so every dir re-parses
    //with fresh content.
    m_symbolIndex.Clear();
    indexStdLib();
    indexConfiguredDirs();
    indexOpenEditorDirs();
}

void MainWindow::indexStdLib() {
    const std::string stdlibDir = langservice::FindStdLibDir(
        QCoreApplication::applicationDirPath().toStdString());
    if (!stdlibDir.empty())
        m_symbolIndex.LoadLibraryDirOnce(stdlibDir);
}

void MainWindow::indexConfiguredDirs() {
    //Global dirs first (relative entries anchor at the user's home).
    //Empty entries are skipped like the compiler's search-path builder
    //skips them — a "" would otherwise anchor at the home dir and pull
    //its .n files into every completion list.
    for (const QString& dir :
            SettingsStore::persisted().librarySearchPaths()) {
        if (dir.trimmed().isEmpty())
            continue;
        m_symbolIndex.LoadLibraryDirOnce(
            resolvedPath(dir, globalPathBase()).toStdString());
    }
    SolutionNode* solution = m_solutionTree->solutionNode();
    if (solution == nullptr)
        return;
    //Then each open project: import paths first, the project's own
    //directory last — mirroring the compiler's search order for those
    //layers (SearchPathArgs puts the project dir after the import
    //paths). Resolve returns the first-indexed symbol, so the index
    //then agrees with the compiler's first-match-wins search. (One
    //package declared in two effective build dirs is a duplicate-
    //package compile error, not a build of either copy; the ordering
    //still matters for layers a given build never consults — another
    //project's dir, an editor's file dir.)
    for (const auto& project : solution->projects()) {
        for (int i = 0; i < project->importPathCount(); ++i)
            m_symbolIndex.LoadLibraryDirOnce(
                project->importPathAt(i).toStdString());
        m_symbolIndex.LoadLibraryDirOnce(project->projectDir().toStdString());
    }
}

void MainWindow::indexOpenEditorDirs() {
    for (const FileEditor* editor : m_editors.editors()) {
        const QString dir =
            QFileInfo(editor->filePath()).absolutePath();
        m_symbolIndex.LoadLibraryDirOnce(dir.toStdString());
    }
}

} // namespace nlang
