/*--- MainWindow.cpp - main window of the NLang IDE ---*/
#include "MainWindow.h"
#include "BreakpointStore.h"
#include "CodeEditor.h"
#include "CompileLogBrowser.h"
#include "DebugClient.h"
#include "HelpBrowser.h"
#include "MainStatusBar.h"
#include "NewFileDialog.h"
#include "ProjectModel.h"
#include "ProjectPropDialog.h"
#include "RecentStore.h"
#include "SolutionTreeModel.h"
#include "FileEditor.h"

#include "ui_MainWindow.h"

#include <nlang_version.h>  // generated from the repo VERSION file

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QTabBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTreeWidgetItem>
#include <QUrl>

namespace nlang {

namespace {

//Characters Windows forbids in file names.
const QString kForbiddenFileNameChars = QStringLiteral("<>:\"/\\|?*");

//Empty when the file NAME is usable as a rename target; otherwise the
//reason (shown as-is, like the domain error strings).
QString fileNameValidationError(const QString& fileName) {
    if (fileName.isEmpty())
        return QStringLiteral("the file name is empty");
    if (fileName == QStringLiteral(".") || fileName == QStringLiteral(".."))
        return QStringLiteral("'.' and '..' are not file names");
    for (const QChar& c : fileName) {
        if (kForbiddenFileNameChars.contains(c))
            return QString("the file name must not contain '%1'").arg(c);
    }
    if (fileName.endsWith('.') || fileName.endsWith(' '))
        return QStringLiteral(
            "the file name must not end with a dot or a space");
    return QString();
}

//True when the two paths spell the same physical file (Windows folds
//case -- the same rule as editorKey and ProjectModel's dedupKey).
bool samePhysicalFile(const QString& pathA, const QString& pathB) {
    const QString a = QFileInfo(pathA).absoluteFilePath();
    const QString b = QFileInfo(pathB).absoluteFilePath();
#ifdef _WIN32
    return a.toLower() == b.toLower();
#else
    return a == b;
#endif
}

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

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_ui(new Ui::MainWindow)
    , m_solutionTree(new SolutionTreeModel(this))
    , m_editors(this)
{
    m_ui->setupUi(this);

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
}

MainWindow::~MainWindow() {
    //Same teardown as closeEvent (whose prompts already ran or don't
    //apply): drop the editors before the members destroy themselves.
    clearEditors();
}

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

//--- solution lifecycle ---

void MainWindow::on_actNewSolution_triggered() {
    if (!closeSolution())
        return;
    m_solutionTree->newSolution(tr("Solution1"));
    m_solutionFilePath.clear();
    m_ui->tvwSolution->expandAll();
    updateMenuState();
}

void MainWindow::on_actOpenSolution_triggered() {
    if (!closeSolution())
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Solution"), QString(),
        tr("NLang Solution (*.nsln);;All Files (*)"));
    if (path.isEmpty())
        return;
    //The close above already ran; the helper's own close is a no-op
    //then (hasSolution() is false).
    openSolutionAtPath(path);
}

bool MainWindow::openSolutionAtPath(const QString& path) {
    //Inside, not in the caller: loadSolution silently replaces an open
    //model, so the unsaved-work gate must be here.
    if (!closeSolution())
        return false;
    QString error;
    if (!m_solutionTree->loadSolution(path, &error)) {
        QMessageBox::critical(this, tr("Error"), error);
        return false;
    }
    m_solutionFilePath = path;
    m_ui->tvwSolution->expandAll();
    updateMenuState();
    noteRecent(path);
    return true;
}

void MainWindow::on_actSaveSolution_triggered() {
    saveSolution();
}

void MainWindow::on_actCloseSolution_triggered() {
    closeSolution();
}

bool MainWindow::saveSolution() {
    if (!m_solutionTree->hasSolution())
        return false;
    QString path = m_solutionFilePath;
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(
            this, tr("Save Solution"), tr("Solution1.nsln"),
            tr("NLang Solution (*.nsln)"));
        if (path.isEmpty())
            return false;
        if (!path.endsWith(".nsln", Qt::CaseInsensitive))
            path += ".nsln";
        m_solutionFilePath = path;
    }
    QString error;
    if (!m_solutionTree->saveSolution(path, &error)) {
        QMessageBox::critical(this, tr("Error"), error);
        return false;
    }
    return true;
}

bool MainWindow::isSolutionModified() const {
    const SolutionNode* solution = m_solutionTree->solutionNode();
    if (solution == nullptr)
        return false;
    if (solution->isDirty())
        return true;
    for (const auto& project : solution->projects())
        if (project->isDirty())
            return true;
    return false;
}

bool MainWindow::closeSolution() {
    if (!m_solutionTree->hasSolution())
        return true;
    if (isSolutionModified()) {
        const auto answer = QMessageBox::question(
            this, tr("Close Solution"),
            tr("The solution or its projects have unsaved changes. "
               "Save before closing?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
            return false;
        if (answer == QMessageBox::Save && !saveSolution())
            return false;
    }
    m_solutionTree->closeSolution();
    m_solutionFilePath.clear();
    //All projects are gone; the browser must not keep resolving through
    //dangling ProjectNode pointers.
    m_ui->txtCompileOut->setProject(nullptr);
    updateMenuState();
    return true;
}

void MainWindow::ensureSolution() {
    if (m_solutionTree->hasSolution())
        return;
    m_solutionTree->newSolution(tr("Solution1"));
    m_ui->tvwSolution->expandAll();
}

//--- projects ---

void MainWindow::on_actNewProject_triggered() {
    ensureSolution();
    ProjectPropDialog dialog(this);
    ProjectNode* project =
        dialog.createProject(*m_solutionTree->solutionNode());
    if (project == nullptr)
        return;
    //createProject mutated the domain node behind the mirror's back.
    m_solutionTree->refresh();
    m_ui->tvwSolution->expandAll();
    selectProject(project);
    updateMenuState();
}

void MainWindow::on_actOpenProject_triggered() {
    ensureSolution();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Project"), QString(),
        tr("NLang Project (*.nproj);;All Files (*)"));
    if (path.isEmpty())
        return;
    //ensureSolution above already ran; the helper's own call is an
    //idempotent no-op then.
    openProjectAtPath(path);
}

ProjectNode* MainWindow::openProjectAtPath(const QString& path) {
    ensureSolution();
    QString error;
    ProjectNode* project = m_solutionTree->openProject(path, &error);
    if (project == nullptr) {
        QMessageBox::warning(this, tr("Error"), error);
        return nullptr;
    }
    selectProject(project);
    m_ui->tvwSolution->expandAll();
    updateMenuState();
    noteRecent(path);
    return project;
}

void MainWindow::on_actSaveProject_triggered() {
    ProjectNode* project = currentProject();
    if (project != nullptr)
        saveProject(*project);
}

void MainWindow::on_actCloseProject_triggered() {
    ProjectNode* project = currentProject();
    if (project == nullptr)
        return;
    if (project->isDirty()) {
        const auto answer = QMessageBox::question(
            this, tr("Close Project"),
            tr("Project '%1' has unsaved changes. Save before closing?")
                .arg(project->name()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
            return;
        if (answer == QMessageBox::Save && !saveProject(*project))
            return;
    }
    //Editors of the project's files stay open -- a file is independent
    //of its project membership.
    m_solutionTree->removeProject(project);
    //The log browser resolved relative ncc paths through this project;
    //the node is gone, so detach it before anything emits a selection.
    if (m_ui->txtCompileOut->project() == project)
        m_ui->txtCompileOut->setProject(nullptr);
    updateMenuState();
}

QString MainWindow::projectFilePath(const ProjectNode* project) const {
    const SolutionNode* solution = m_solutionTree->solutionNode();
    for (int i = 0; solution != nullptr && i < solution->projectCount();
         ++i) {
        if (solution->projects()[static_cast<size_t>(i)].get() == project)
            return solution->absoluteProjectPath(i);
    }
    return QString();
}

bool MainWindow::saveProject(ProjectNode& project) {
    const QString path = projectFilePath(&project);
    if (path.isEmpty())
        return false;
    QString error;
    if (!project.save(path, &error)) {
        QMessageBox::critical(this, tr("Error"), error);
        return false;
    }
    return true;
}

void MainWindow::selectProject(ProjectNode* project) {
    QAbstractItemModel* model = m_ui->tvwSolution->model();
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex solutionIndex = model->index(r, 0);
        for (int p = 0; p < model->rowCount(solutionIndex); ++p) {
            const QModelIndex projectIndex =
                model->index(p, 0, solutionIndex);
            SolutionTreeItem* item = m_solutionTree->itemAt(projectIndex);
            if (item != nullptr && item->project() == project) {
                m_ui->tvwSolution->setCurrentIndex(projectIndex);
                return;
            }
        }
    }
}

void MainWindow::selectFile(FileNode* file) {
    //One level deeper than selectProject; same tiny-tree scan. Expand
    //first: a current index inside a collapsed project row is selected
    //but never seen (every other caller expands right after selecting).
    m_ui->tvwSolution->expandAll();
    QAbstractItemModel* model = m_ui->tvwSolution->model();
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex solutionIndex = model->index(r, 0);
        for (int p = 0; p < model->rowCount(solutionIndex); ++p) {
            const QModelIndex projectIndex =
                model->index(p, 0, solutionIndex);
            for (int f = 0; f < model->rowCount(projectIndex); ++f) {
                const QModelIndex fileIndex =
                    model->index(f, 0, projectIndex);
                SolutionTreeItem* item =
                    m_solutionTree->itemAt(fileIndex);
                if (item != nullptr && item->file() == file) {
                    m_ui->tvwSolution->setCurrentIndex(fileIndex);
                    return;
                }
            }
        }
    }
}

//--- files / editors ---

void MainWindow::on_actNewFile_triggered() {
    //The dialog defaults AND the tree join target follow this: selected
    //project, sole project as fallback, standalone without a solution.
    ProjectNode* project = targetProjectForNewFile();
    NewFileDialog dialog(this);
    dialog.init(project);
    QString filePath;
    if (!dialog.getFilePath(filePath))
        return;
    if (!editNewFile(filePath))
        return;
    if (project == nullptr)
        return;
    FileNode* file = m_solutionTree->addFile(project, filePath);
    if (file == nullptr)
        QMessageBox::warning(
            this, tr("Error"),
            tr("'%1' is already part of the project.").arg(filePath));
    else
        selectFile(file);  // file-scoped actions come alive
}

void MainWindow::on_actOpenFile_triggered() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open File"), QString(),
        tr("NLang Source (*.n);;All Files (*)"));
    if (!path.isEmpty())
        editExistingFile(path);
}

void MainWindow::on_actCloseFile_triggered() {
    FileEditor* editor = currentEditor();
    if (editor != nullptr && closeEditor(editor))
        closeEditorTab(editor);
}

void MainWindow::on_actSaveFile_triggered() {
    FileEditor* editor = currentEditor();
    if (editor != nullptr)
        saveEditor(editor);
}

void MainWindow::on_actSaveFileAs_triggered() {
    FileEditor* editor = currentEditor();
    if (editor == nullptr)
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save File As"), editor->filePath(),
        tr("NLang Source (*.n);;All Files (*)"));
    if (path.isEmpty())
        return;
    if (!editor->saveAs(QFileInfo(path).absoluteFilePath()))
        QMessageBox::warning(this, tr("Error"), editor->lastError());
    else {
        noteRecent(editor->filePath());  //the saved-as path joins the list
        onEditorSaveStateChanged(editor);  // new file name on the tab
    }
}

void MainWindow::on_actSaveAll_triggered() {
    for (FileEditor* editor : m_editors.editors()) {
        if (editor->dirty() && !saveEditor(editor))
            return;
    }
    if (m_solutionTree->hasSolution() && isSolutionModified())
        saveSolution();
}

void MainWindow::on_actQuit_triggered() {
    close();
}

void MainWindow::addEditorTab(FileEditor* editor) {
    QWidget* widget = editor->widget();
    m_ui->tabCodes->addTab(widget, widget->windowTitle());
    m_ui->tabCodes->setCurrentWidget(widget);
    connect(editor, &FileEditor::positionInfoChanged, this,
            &MainWindow::onEditorPositionChanged);
    //Gutter clicks join the F9 path; the stored table paints the fresh
    //editor's dots.
    if (CodeEditor* code = qobject_cast<CodeEditor*>(widget)) {
        connect(code, &CodeEditor::breakpointToggled, this,
                &MainWindow::onBreakpointGutterClicked);
        refreshBreakpointMarkers();
    }
}

void MainWindow::editExistingFile(const QString& filePath) {
    FileEditor* editor = m_editors.open(filePath);
    if (editor == nullptr) {
        QMessageBox::warning(this, tr("Error"), m_editors.lastError());
        return;
    }
    noteRecent(editor->filePath());  //normalized absolute path
    QWidget* widget = editor->widget();
    if (m_ui->tabCodes->indexOf(widget) < 0)
        addEditorTab(editor);
    else
        m_ui->tabCodes->setCurrentWidget(widget);
}

bool MainWindow::editNewFile(const QString& filePath) {
    FileEditor* editor = m_editors.openNew(filePath);
    if (editor == nullptr) {
        QMessageBox::warning(this, tr("Error"), m_editors.lastError());
        return false;
    }
    addEditorTab(editor);
    noteRecent(editor->filePath());
    return true;
}

bool MainWindow::saveEditor(FileEditor* editor) {
    if (!editor->save()) {
        QMessageBox::warning(this, tr("Error"), editor->lastError());
        return false;
    }
    return true;
}

bool MainWindow::closeEditor(FileEditor* editor) {
    if (editor->dirty()) {
        const auto answer = QMessageBox::question(
            this, tr("Close File"),
            tr("'%1' has been modified. Save changes?")
                .arg(QFileInfo(editor->filePath()).fileName()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
            return false;
        if (answer == QMessageBox::Save && !saveEditor(editor))
            return false;
    }
    return true;
}

void MainWindow::closeEditorTab(FileEditor* editor) {
    const int index = m_ui->tabCodes->indexOf(editor->widget());
    if (index >= 0)
        m_ui->tabCodes->removeTab(index);
    m_editors.remove(editor);
    //A removed CURRENT tab fires currentChanged inside removeTab, while
    //the editor is still registered -- only this late snapshot reflects
    //the closed editor's membership (a background close fires no
    //currentChanged at all).
    updateMenuState();
}

void MainWindow::clearEditors() {
    //Deleting an editor deletes its tab page, which fires tab signals
    //on a half-torn state -- block them for the sweep.
    QSignalBlocker blocker(m_ui->tabCodes);
    m_editors.clear();
    refreshStandaloneFiles();
}

//--- 项目 menu ---

void MainWindow::on_actAddExistFile_triggered() {
    ProjectNode* project = currentProject();
    if (project == nullptr)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Add Existing File"), project->projectDir(),
        tr("NLang Source (*.n);;All Files (*)"));
    if (path.isEmpty())
        return;
    FileNode* file = m_solutionTree->addFile(project, path);
    if (file == nullptr)
        QMessageBox::warning(
            this, tr("Error"),
            tr("'%1' is already part of the project.").arg(path));
    else
        selectFile(file);  // file-scoped actions come alive
}

void MainWindow::on_actAddNewFile_triggered() {
    ProjectNode* project = currentProject();
    if (project == nullptr)
        return;
    NewFileDialog dialog(this);
    dialog.init(project);
    QString filePath;
    if (!dialog.getFilePath(filePath))
        return;
    if (!editNewFile(filePath))
        return;
    FileNode* file = m_solutionTree->addFile(project, filePath);
    if (file == nullptr)
        QMessageBox::warning(
            this, tr("Error"),
            tr("'%1' is already part of the project.").arg(filePath));
    else
        selectFile(file);  // file-scoped actions come alive
}

void MainWindow::on_actRemoveFile_triggered() {
    FileNode* file = currentFile();
    if (file != nullptr)
        m_solutionTree->removeFile(file);
}

void MainWindow::on_actProjectProp_triggered() {
    ProjectNode* project = currentProject();
    if (project == nullptr)
        return;
    ProjectPropDialog dialog(this);
    if (dialog.editProject(*project))
        m_solutionTree->refresh();
}

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

    //Synchronous build (EN did the same): ncc writes diagnostics in the
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

void MainWindow::showOutputPage(QWidget* page) {
    //setChecked alone doesn't emit triggered(), so the pane is shown
    //here explicitly; the checked state keeps the view menu in sync.
    m_ui->actViewOutput->setChecked(true);
    m_ui->tabOutput->setVisible(true);
    m_ui->tabOutput->setCurrentWidget(page);
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

bool MainWindow::startDebugSession() {
    QString modulePath;
    if (ProjectNode* project = currentProject()) {
        if (!buildProject(*project))
            return false;
        modulePath = outputFilePath(*project);
        m_debugBaseDir = project->projectDir();
    } else {
        const QString standalone = currentStandaloneTarget();
        if (standalone.isEmpty())
            return false;   // the action was disabled without a target
        if (!buildStandaloneFile(standalone))
            return false;
        modulePath = standaloneNmodPath(standalone);
        m_debugBaseDir = QFileInfo(standalone).absolutePath();
    }
    if (!QFileInfo::exists(modulePath))
        return false;

    //One DebugClient per session: Ended is terminal there, so every
    //session creates a fresh instance (parented to the window, which
    //also gives tests a findChildren seam).
    m_debugClient = std::make_unique<DebugClient>(toolPath("ndb"), this);
    m_debugStopRequested = false;
    m_breakpointIds.clear();
    m_pendingBreakpointChanges.clear();
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

    clearDebugViews();
    m_ui->txtExecuteOut->clear();
    showOutputPage(m_ui->tabDebug);
    //Same CWD policy as Run: an example writing files stays inside its
    //module's directory.
    m_debugClient->setWorkingDirectory(QFileInfo(modulePath).absolutePath());
    if (!m_debugClient->launch(modulePath)) {
        endDebugSession();
        return false;
    }
    //Prelude: every stored breakpoint, then the throw toggle. `run` is
    //deferred by the client until hello AND every bp receipt arrived,
    //so issuing all three back to back has no handshake race.
    for (const QString& file : m_breakpoints.files())
        for (int line : m_breakpoints.linesOf(file))
            m_debugClient->addBreakpoint(file, line);
    m_debugClient->setBreakOnThrow(m_ui->chkBreakOnThrow->isChecked());
    m_debugClient->run();
    setDebugStatus(tr("Debug started"));
    updateMenuState();
    return true;
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
    if (bound && id > 0) {
        if (m_breakpoints.contains(file, line)) {
            m_breakpointIds[{BreakpointStore::normalizedKey(file), line}] = id;
        } else {
            //A receipt can lag its own undo (a Running-phase toggle
            //replays the add and the remove back to back at the next
            //stop, and the remove finds no wire id yet). Retire the
            //breakpoint ndb just materialized -- left alone it ghosts
            //(spurious stops) and the stale id duplicates on a re-toggle.
            m_debugClient->deleteBreakpoint(id);
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
    showOutputPage(m_ui->tabDebug);
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
    if (m_debugClient != nullptr) {
        //Deferred delete: this usually runs inside one of the client's
        //own signal handlers, so the object must outlive the emit.
        //release() hands the ownership to the event loop.
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
    for (const PendingBreakpointChange& change :
            m_pendingBreakpointChanges)
        sendBreakpointChange(change.filePath, change.line, change.add);
    m_pendingBreakpointChanges.clear();
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

QString MainWindow::outputFilePath(const ProjectNode& project) const {
    //Empty outputDir = the project directory (the .nproj default).
    const QString dir = project.outputDir().isEmpty()
        ? project.projectDir()
        : project.absolutePathOf(project.outputDir());
    return QDir(dir).filePath(project.name() + ".nmod");
}

QString MainWindow::standaloneNmodPath(const QString& filePath) const {
    //Per-user temp area: the examples dir may be read-only (installed
    //layout) and we never write next to the source.
    const QString dir = QDir(QDir::temp()).filePath("nlang-nide");
    QDir().mkpath(dir);
    return QDir(dir).filePath(
        QFileInfo(filePath).completeBaseName() + ".nmod");
}

QString MainWindow::toolPath(const QString& toolName) const {
    //ncc/nvm sit next to nide.exe (a CMake POST_BUILD step arranges
    //that for developer builds; deployments keep them together).
    return QDir(QCoreApplication::applicationDirPath()).filePath(toolName);
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

//--- view / help ---

void MainWindow::on_actViewSolution_triggered(bool checked) {
    m_ui->dckSolution->setVisible(checked);
}

void MainWindow::on_actViewCodeEditor_triggered(bool checked) {
    m_ui->tabCodes->setVisible(checked);
}

void MainWindow::on_actViewOutput_triggered(bool checked) {
    m_ui->tabOutput->setVisible(checked);
}

void MainWindow::on_actViewToolBar_triggered(bool checked) {
    m_ui->mainToolBar->setVisible(checked);
}

void MainWindow::on_actHelpAbout_triggered() {
    //The version line comes from the generated nlang_version.h (single
    //source: the repository VERSION file). The anchor makes Qt treat
    //the whole text as rich text, so line breaks must be <br> -- a raw
    //\n collapses to a space. The message box label opens external
    //links by default and renders anchors as blue underlined text.
    QMessageBox::about(
        this, tr("About NLang IDE"),
        tr("NLang IDE %1<br>The integrated development environment for "
           "the NLang scripting language.<br><br>"
           "<a href=\"https://github.com/dliting/nlang\">"
           "https://github.com/dliting/nlang</a>")
            .arg(QLatin1String(NLANG_VERSION)));
}

namespace {
//How far above the executable to search for the docs site. The
//installed layout resolves at hop 1 (bin/../docs/site); the dev nide
//exe (build-ide/src/tools/nide/Release) and the test exes
//(<build>/tests/Release) sit deeper -- cap covers both.
const int MAX_DOC_SITE_HOPS = 6;
} // namespace

QString MainWindow::locateHelpPage(const QString& documentPagePath) {
    QDir dir = QCoreApplication::applicationDirPath();
    for (int hop = 0; hop < MAX_DOC_SITE_HOPS; ++hop) {
        //use_directory_urls:false output: flat .html files
        //(e.g. "language-spec.html", later "language-spec/overview.html").
        const QString candidate = dir.absoluteFilePath(
            "docs/site/" + documentPagePath + ".html");
        if (QFileInfo::exists(candidate))
            return candidate;
        if (!dir.cdUp())
            break;
    }
    return QString();
}

void MainWindow::on_actHelpGettingStarted_triggered() {
    openHelpDocument(QStringLiteral("getting-started/what-is-nolang"));
}

void MainWindow::on_actHelpLanguageSpec_triggered() {
    openHelpDocument(QStringLiteral("language-spec/overview"));
}

void MainWindow::on_actHelpVmArch_triggered() {
    openHelpDocument(QStringLiteral("vm-architecture/overview"));
}

void MainWindow::openHelpDocument(const QString& documentPagePath) {
    const QString page = locateHelpPage(documentPagePath);
    if (page.isEmpty()) {
        QMessageBox::warning(
            this, tr("Error"),
            tr("The document '%1' was not found next to the IDE "
               "installation.").arg(documentPagePath));
        return;
    }
    //Embedded viewer instead of the system browser. The browser
    //deletes itself on close (m_helpBrowser self-nulls), so every
    //entry re-creates it; while it is open, entries reuse it.
    if (m_helpBrowser.isNull())
        m_helpBrowser = new HelpBrowser(this);
    m_helpBrowser->openPage(QUrl::fromLocalFile(page));
}

//--- widget slots ---

void MainWindow::on_tabCodes_tabCloseRequested(int index) {
    FileEditor* editor =
        m_editors.findEditor(m_ui->tabCodes->widget(index));
    if (editor != nullptr && closeEditor(editor))
        closeEditorTab(editor);
}

void MainWindow::on_tabCodes_currentChanged(int) {
    FileEditor* editor = currentEditor();
    m_ui->statusBar->showPosition(
        editor != nullptr ? editor->positionInfo() : QString());
    updateMenuState();
}

void MainWindow::on_tvwSolution_doubleClicked(const QModelIndex& index) {
    SolutionTreeItem* item = m_solutionTree->itemAt(index);
    if (item == nullptr)
        return;
    if (item->nodeType() == SolutionTreeItem::NT_File)
        editExistingFile(item->file()->absolutePath());
    else if (item->nodeType() == SolutionTreeItem::NT_StandaloneFile)
        editExistingFile(item->standalonePath());
}

void MainWindow::on_tvwSolution_customContextMenuRequested(const QPoint& pos) {
    const QModelIndex index = m_ui->tvwSolution->indexAt(pos);
    if (!index.isValid())
        return;
    m_ui->tvwSolution->setCurrentIndex(index);
    SolutionTreeItem* item = m_solutionTree->itemAt(index);
    if (item == nullptr)
        return;

    //File-scoped entries: a standalone row only gets Open (no domain
    //node behind it to rename or remove); the project/solution/group
    //rows show the menu with everything disabled (the tree knows no
    //project rename yet).
    const bool isProjectFile =
        item->nodeType() == SolutionTreeItem::NT_File;
    const bool isStandalone =
        item->nodeType() == SolutionTreeItem::NT_StandaloneFile;
    QMenu menu(this);
    QAction* openAction = menu.addAction(tr("Open"));
    QAction* renameAction = menu.addAction(tr("Rename (F2)"));
    QAction* removeAction = menu.addAction(tr("Remove from Project"));
    openAction->setEnabled(isProjectFile || isStandalone);
    renameAction->setEnabled(isProjectFile);
    removeAction->setEnabled(isProjectFile);

    QAction* chosen =
        menu.exec(m_ui->tvwSolution->viewport()->mapToGlobal(pos));
    if (chosen == openAction)
        editExistingFile(isProjectFile ? item->file()->absolutePath()
                                       : item->standalonePath());
    else if (chosen == renameAction) {
        //After exec: opening the editor inside the exec stack would
        //have the menu's closing focus churn destroy it immediately.
        m_ui->tvwSolution->edit(index);  // the inline editor (like F2)
    } else if (chosen == removeAction)
        m_ui->actRemoveFile->trigger();
}

void MainWindow::showTabContextMenu(const QPoint& pos) {
    QTabBar* bar = m_ui->tabCodes->tabBar();
    const int index = bar->tabAt(pos);
    QWidget* widget = m_ui->tabCodes->widget(index);
    FileEditor* editor = m_editors.findEditor(widget);
    if (editor == nullptr)
        return;

    QMenu menu(this);
    QAction* saveAction = menu.addAction(tr("Save"));
    QAction* saveAsAction = menu.addAction(tr("Save As..."));
    menu.addSeparator();
    QAction* renameAction = menu.addAction(tr("Rename..."));
    QAction* closeAction = menu.addAction(tr("Close"));
    QAction* closeOthersAction = menu.addAction(tr("Close Others"));

    QAction* chosen = menu.exec(bar->mapToGlobal(pos));
    if (chosen == saveAction || chosen == saveAsAction ||
        chosen == closeAction) {
        //Reuse the existing actions: they act on the CURRENT tab, so
        //make the right-clicked tab current first.
        m_ui->tabCodes->setCurrentIndex(index);
        if (chosen == saveAction)
            m_ui->actSaveFile->trigger();
        else if (chosen == saveAsAction)
            m_ui->actSaveFileAs->trigger();
        else
            m_ui->actCloseFile->trigger();
    } else if (chosen == renameAction) {
        const QString oldPath = editor->filePath();
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename File"), tr("New name:"), QLineEdit::Normal,
            QFileInfo(oldPath).fileName(), &accepted);
        if (accepted && !name.trimmed().isEmpty())
            renameFileEverywhere(oldPath, name.trimmed(),
                                 findFileNodeByPath(oldPath));
    } else if (chosen == closeOthersAction) {
        closeOtherEditorTabs(index);
    }
}

void MainWindow::closeOtherEditorTabs(int keepIndex) {
    QWidget* keep = m_ui->tabCodes->widget(keepIndex);
    if (keep == nullptr)
        return;
    //Descending walk anchored on widgets: closing later tabs first
    //leaves earlier indexes alone, and a vetoed (Cancel) close just
    //keeps that tab while the sweep continues.
    for (int i = m_ui->tabCodes->count() - 1; i >= 0; --i) {
        QWidget* widget = m_ui->tabCodes->widget(i);
        if (widget == keep)
            continue;
        FileEditor* editor = m_editors.findEditor(widget);
        if (editor != nullptr && closeEditor(editor))
            closeEditorTab(editor);
    }
    m_ui->tabCodes->setCurrentWidget(keep);
}

void MainWindow::on_dckSolution_visibilityChanged(bool visible) {
    //The dock's close button must keep the view action in sync.
    m_ui->actViewSolution->setChecked(visible);
}

//--- non-widget slots ---

void MainWindow::onEditorSaveStateChanged(FileEditor* editor) {
    const int index = m_ui->tabCodes->indexOf(editor->widget());
    if (index >= 0)
        m_ui->tabCodes->setTabText(index,
                                   editor->widget()->windowTitle());
    updateMenuState();
}

void MainWindow::onEditorPositionChanged() {
    FileEditor* editor = qobject_cast<FileEditor*>(sender());
    if (editor != nullptr && editor == currentEditor())
        m_ui->statusBar->showPosition(editor->positionInfo());
}

void MainWindow::onSolutionSelectionChanged() {
    updateMenuState();
}

//--- file rename pipeline ---

void MainWindow::onFileRenameRequested(FileNode* file,
                                        const QString& newName) {
    renameFileEverywhere(file->absolutePath(), newName, file);
}

bool MainWindow::renameFileEverywhere(QString oldPath,
                                      const QString& newFileName,
                                      FileNode* trackedFile) {
    const QString reason = fileNameValidationError(newFileName);
    if (!reason.isEmpty()) {
        QMessageBox::warning(this, tr("Error"), reason);
        return false;
    }

    const QString newPath =
        QFileInfo(QFileInfo(oldPath).dir().filePath(newFileName))
            .absoluteFilePath();
    //Only the byte-identical name is a no-op. A case-only variant
    //(main.n -> Main.n) renames for real -- the domain and editor
    //layers are built for it -- so the exists-check must not trip on
    //the file itself (Windows exists() folds case).
    if (newPath == QFileInfo(oldPath).absoluteFilePath())
        return true;  // the same name: nothing to move
    const bool caseVariantOnly = samePhysicalFile(oldPath, newPath);

    if (!caseVariantOnly && QFileInfo::exists(newPath)) {
        QMessageBox::warning(this, tr("Error"),
                             tr("'%1' already exists.").arg(newPath));
        return false;
    }

    //Persist dirty edits to the OLD path first: the disk rename then
    //carries them to the new name, and a failed save aborts with
    //nothing moved.
    FileEditor* editor = m_editors.find(oldPath);
    if (editor != nullptr && editor->dirty() && !saveEditor(editor))
        return false;

    if (!QFile::rename(oldPath, newPath)) {
        QMessageBox::warning(
            this, tr("Error"),
            tr("Cannot rename '%1' to '%2'.").arg(oldPath, newPath));
        return false;
    }

    QString error;
    if (trackedFile != nullptr &&
        !m_solutionTree->renameFile(trackedFile, newPath, &error)) {
        //The domain rejected the move (a duplicate in another casing
        //etc.): roll the disk back so both layers stay consistent.
        QFile::rename(newPath, oldPath);
        QMessageBox::warning(this, tr("Error"), error);
        return false;
    }

    if (editor != nullptr) {
        editor->onExternalRename(newPath);
        onEditorSaveStateChanged(editor);  // new file name on the tab
    }
    if (trackedFile != nullptr)
        selectFile(trackedFile);
    //In-place swap in the recent list (rank kept); a rename is not an
    //open, so an unlisted file does not join the list.
    m_recent.replace(oldPath, newPath);
    saveRecent();
    //Breakpoints follow the file too (persistence re-keyed in place). A
    //live session keeps its already-sent wire ids and ndb's recorded
    //paths on the old name until the session ends -- accepted staleness
    //in v1 (breakpoint hits are a line snapshot, not a live path watch).
    m_breakpoints.rename(oldPath, newPath);
    saveBreakpoints();
    refreshBreakpointMarkers();
    return true;
}

FileNode* MainWindow::findFileNodeByPath(const QString& filePath) const {
    SolutionNode* solution = m_solutionTree->solutionNode();
    for (int i = 0; solution != nullptr && i < solution->projectCount();
         ++i) {
        ProjectNode* project =
            solution->projects()[static_cast<size_t>(i)].get();
        for (const std::unique_ptr<FileNode>& file : project->files()) {
            if (samePhysicalFile(file->absolutePath(), filePath))
                return file.get();
        }
    }
    return nullptr;
}

//--- standalone files ---

QStringList MainWindow::standaloneEditorPaths() const {
    QStringList paths;
    for (FileEditor* editor : m_editors.editors()) {
        if (findFileNodeByPath(editor->filePath()) == nullptr)
            paths << editor->filePath();
    }
    return paths;
}

void MainWindow::refreshStandaloneFiles() {
    //Expand the group's chain only when its membership actually
    //changed: this refresh runs on every menu-state update (tab
    //switch, save, tree selection...), an unconditional expand would
    //keep re-opening whatever the user had collapsed.
    if (!m_solutionTree->setStandaloneFiles(standaloneEditorPaths()))
        return;
    if (SolutionTreeItem* group = m_solutionTree->standaloneGroupItem()) {
        const QModelIndex groupIndex = m_solutionTree->indexFromItem(group);
        m_ui->tvwSolution->expand(groupIndex);
        for (QModelIndex parent = groupIndex.parent(); parent.isValid();
             parent = parent.parent())
            m_ui->tvwSolution->expand(parent);
    }
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
    //is live exactly while the process runs.
    const bool running =
        m_executed.state() != QProcess::NotRunning;
    m_ui->actStartRunning->setEnabled(canBuild && !running && !debugLive);
    m_ui->actStopRunning->setEnabled(running);

    //Debug lifecycle: F5 doubles as Continue while paused; Stop and the
    //steps track the session windows; the checkbox grays out while the
    //program runs (the wire accepts the toggle only in the command
    //windows, Launching/Stopped).
    const bool debugStopped = debugLive
        && m_debugClient->state() == DebugClient::State::Stopped;
    m_ui->actStartDebug->setEnabled(
        (canBuild && !debugLive) || debugStopped);
    m_ui->actStopDebug->setEnabled(debugLive);
    m_ui->actStepInto->setEnabled(debugStopped);
    m_ui->actStepOver->setEnabled(debugStopped);
    m_ui->actStepOut->setEnabled(debugStopped);
    m_ui->actToggleBreakpoint->setEnabled(hasEditor);
    m_ui->chkBreakOnThrow->setEnabled(
        !debugLive || m_debugClient->state() != DebugClient::State::Running);

    const bool hasSolution = m_solutionTree->hasSolution();
    m_ui->actSaveSolution->setEnabled(hasSolution);
    m_ui->actCloseSolution->setEnabled(hasSolution);
    m_ui->actNewSolution->setEnabled(true);
    m_ui->actOpenSolution->setEnabled(true);
}

//--- recent list ---

void MainWindow::noteRecent(const QString& absolutePath) {
    m_recent.push(absolutePath);
    saveRecent();
}

void MainWindow::saveRecent() {
    QSettings settings;
    m_recent.save(settings);
}

void MainWindow::rebuildRecentMenu() {
    QMenu* menu = m_ui->menuRecent;
    menu->clear();
    //Existing-on-disk entries only: a missing file is hidden but
    //stays stored until a newer entry evicts it.
    QStringList visible;
    QSet<QString> names;
    QSet<QString> duplicated;
    for (const QString& path : m_recent.entries()) {
        if (!QFileInfo::exists(path))
            continue;
        visible << path;
        const QString name = QFileInfo(path).fileName();
        if (names.contains(name))
            duplicated << name;
        names << name;
    }
    for (const QString& path : visible) {
        const QFileInfo info(path);
        QString text = info.fileName();
        if (duplicated.contains(text))
            text += QStringLiteral(" (") + info.dir().dirName() +
                    QStringLiteral(")");
        //Keep & out of the mnemonic role.
        text.replace(QLatin1Char('&'), QStringLiteral("&&"));
        QAction* action = menu->addAction(
            recentEntryIcon(info), text, this,
            &MainWindow::onRecentEntryTriggered);
        action->setToolTip(path);
        action->setData(path);
    }
    menu->addSeparator();
    menu->addAction(tr("Clear Recent List"), this,
                    &MainWindow::onClearRecentTriggered);
    //Shown with something clickable in it, hidden otherwise.
    menu->menuAction()->setVisible(!visible.isEmpty());
}

QIcon MainWindow::recentEntryIcon(const QFileInfo& info) const {
    //Container metaphor: solution = drive, project = folder, file =
    //document. QFileIconProvider cannot tell custom extensions apart.
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("nsln"))
        return style()->standardIcon(QStyle::SP_DriveHDIcon);
    if (suffix == QStringLiteral("nproj"))
        return style()->standardIcon(QStyle::SP_DirIcon);
    return style()->standardIcon(QStyle::SP_FileIcon);
}

void MainWindow::onRecentEntryTriggered() {
    QAction* action = qobject_cast<QAction*>(sender());
    if (action == nullptr)
        return;
    const QString path = action->data().toString();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("nsln"))
        openSolutionAtPath(path);
    else if (suffix == QStringLiteral("nproj"))
        openProjectAtPath(path);
    else
        editExistingFile(path);
}

void MainWindow::onClearRecentTriggered() {
    m_recent.clear();
    saveRecent();
    rebuildRecentMenu();
}

} // namespace nlang
