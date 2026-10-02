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
#include "SettingsStore.h"
#include "SolutionTreeModel.h"

#include "ui_MainWindow.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QMenu>
#include <QProcess>
#include <QSettings>
#include <QSplitter>
#include <QTabBar>

namespace nlang {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_ui(new Ui::MainWindow)
    , m_solutionTree(new SolutionTreeModel(this))
    , m_editors(this)
{
    m_ui->setupUi(this);
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

    //One merged channel: nvm/ncc output (and our own exit lines) all
    //arrive through readyReadStandardOutput.
    m_executed.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_executed, &QProcess::readyReadStandardOutput, this,
            &MainWindow::onExecOutput);
    connect(&m_executed,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onExecFinished);
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
    //remains the default (the .nproj default).
    return SettingsStore::resolveProjectNmodPath(
        project.outputDir(), project.projectDir(),
        SettingsStore::persisted().buildOutputDir(), project.name());
}

QString MainWindow::standaloneNmodPath(const QString& filePath) const {
    //The global build output directory redirects the per-stem slot;
    //unset keeps the per-user temp area (the examples dir may be
    //read-only in the installed layout, and we never write next to
    //the source).
    const QString path = SettingsStore::resolveStandaloneNmodPath(
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

void MainWindow::updateMenuState() {
    //First line on purpose: the group must mirror the editors BEFORE
    //the enablement reads below consult the (possibly just-mutated)
    //tree selection. Every editor/solution mutation converges here.
    refreshStandaloneFiles();
    const bool hasEditor = currentEditor() != nullptr;
    const ProjectNode* project = currentProject();
    const FileNode* file = currentFile();

    m_ui->actNewFile->setEnabled(true);
    m_ui->actOpenFile->setEnabled(true);
    m_ui->actSaveFile->setEnabled(hasEditor);
    m_ui->actSaveFileAs->setEnabled(hasEditor);
    m_ui->actCloseFile->setEnabled(hasEditor);

    const bool hasProject = project != nullptr;
    m_ui->actNewProject->setEnabled(true);
    m_ui->actOpenProject->setEnabled(true);
    m_ui->actSaveProject->setEnabled(hasProject);
    m_ui->actCloseProject->setEnabled(hasProject);
    m_ui->actAddExistFile->setEnabled(hasProject);
    m_ui->actAddNewFile->setEnabled(hasProject);
    m_ui->actRemoveFile->setEnabled(file != nullptr);
    m_ui->actProjectProp->setEnabled(hasProject);
    //A project OR a standalone .n target can be built. While a debug
    //session is live, Build/Run stay off: a mid-session rebuild would
    //rewrite the very .nmod the debugger is executing (bytecode offsets
    //shift under the session) and a Run child would interleave its
    //output on the shared run page.
    const bool hasStandaloneTarget = !currentStandaloneTarget().isEmpty();
    const bool canBuild = hasProject || hasStandaloneTarget;
    const bool debugLive = debugSessionLive();
    m_ui->actBuild->setEnabled(canBuild && !debugLive);

    //Run lifecycle: Start needs a build target AND an idle process; Stop
    //is live exactly while the process runs. The input row follows the
    //same two consumers: the run child's stdin pipe or the debug
    //session's machine channel.
    const bool running =
        m_executed.state() != QProcess::NotRunning;
    m_ui->actStartRunning->setEnabled(canBuild && !running && !debugLive);
    m_ui->actStopRunning->setEnabled(running);
    m_ui->editStdin->setEnabled(running || debugLive);
    m_ui->btnStdinSend->setEnabled(running || debugLive);

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

} // namespace nlang
