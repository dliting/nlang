/*--- MainWindow.cpp - main window of the NLang IDE ---*/
#include "MainWindow.h"
#include "CodeEditor.h"
#include "CompileLogBrowser.h"
#include "MainStatusBar.h"
#include "NewFileDialog.h"
#include "ProjectModel.h"
#include "ProjectPropDialog.h"
#include "SolutionTreeModel.h"
#include "FileEditor.h"

#include "ui_MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabBar>
#include <QTextBlock>
#include <QTextCursor>
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
    QString error;
    if (!m_solutionTree->loadSolution(path, &error)) {
        QMessageBox::critical(this, tr("Error"), error);
        return;
    }
    m_solutionFilePath = path;
    m_ui->tvwSolution->expandAll();
    updateMenuState();
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
    QString error;
    ProjectNode* project = m_solutionTree->openProject(path, &error);
    if (project == nullptr) {
        QMessageBox::warning(this, tr("Error"), error);
        return;
    }
    selectProject(project);
    m_ui->tvwSolution->expandAll();
    updateMenuState();
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
    else
        onEditorSaveStateChanged(editor);  // new file name on the tab
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
}

void MainWindow::editExistingFile(const QString& filePath) {
    FileEditor* editor = m_editors.open(filePath);
    if (editor == nullptr) {
        QMessageBox::warning(this, tr("Error"), m_editors.lastError());
        return;
    }
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

void MainWindow::buildProject(ProjectNode& project) {
    //Save the editors of this project's files so ncc sees the edits.
    for (const auto& file : project.files()) {
        FileEditor* editor = m_editors.find(file->absolutePath());
        if (editor != nullptr && editor->dirty() &&
            !saveEditor(editor))
            return;
    }
    if (project.isDirty() && !saveProject(project))
        return;

    m_ui->txtCompileOut->setProject(&project);
    m_ui->txtCompileOut->clear();
    showOutputPage(m_ui->tabCompileOut);

    const QString output = outputFilePath(project);
    if (!QDir().mkpath(QFileInfo(output).absolutePath())) {
        QMessageBox::warning(
            this, tr("Error"),
            tr("Cannot create the output directory '%1'.")
                .arg(QFileInfo(output).absolutePath()));
        return;
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
    QMessageBox::about(
        this, tr("About NLang IDE"),
        tr("NLang IDE\nThe integrated development environment for the "
           "NLang scripting language."));
}

namespace {
//How far above the executable to search for the docs site. The
//installed layout resolves at hop 1 (bin/../docs/site); the dev nide
//exe (build-ide/src/tools/nide/Release) and the test exes
//(<build>/tests/Release) sit deeper -- cap covers both.
const int MAX_DOC_SITE_HOPS = 6;
} // namespace

QString MainWindow::locateHelpPage(const QString& documentBaseName) {
    QDir dir = QCoreApplication::applicationDirPath();
    for (int hop = 0; hop < MAX_DOC_SITE_HOPS; ++hop) {
        const QString candidate = dir.absoluteFilePath(
            "docs/site/" + documentBaseName + "/index.html");
        if (QFileInfo::exists(candidate))
            return candidate;
        if (!dir.cdUp())
            break;
    }
    return QString();
}

void MainWindow::on_actHelpGettingStarted_triggered() {
    openHelpDocument(QStringLiteral("nlang-getting-started"));
}

void MainWindow::on_actHelpLanguageSpec_triggered() {
    openHelpDocument(QStringLiteral("language-spec"));
}

void MainWindow::on_actHelpVmArch_triggered() {
    openHelpDocument(QStringLiteral("vm-architecture"));
}

void MainWindow::openHelpDocument(const QString& documentBaseName) {
    const QString page = locateHelpPage(documentBaseName);
    if (page.isEmpty()) {
        //Same notice the old in-app viewer showed, now modal.
        QMessageBox::warning(
            this, tr("Error"),
            tr("The document '%1' was not found next to the IDE "
               "installation.").arg(documentBaseName));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(page));
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

bool MainWindow::renameFileEverywhere(const QString& oldPath,
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
    //A project OR a standalone .n target can be built.
    const bool hasStandaloneTarget = !currentStandaloneTarget().isEmpty();
    const bool canBuild = hasProject || hasStandaloneTarget;
    m_ui->actBuild->setEnabled(canBuild);

    //Run lifecycle: Start needs a build target AND an idle process; Stop
    //is live exactly while the process runs.
    const bool running =
        m_executed.state() != QProcess::NotRunning;
    m_ui->actStartRunning->setEnabled(canBuild && !running);
    m_ui->actStopRunning->setEnabled(running);

    const bool hasSolution = m_solutionTree->hasSolution();
    m_ui->actSaveSolution->setEnabled(hasSolution);
    m_ui->actCloseSolution->setEnabled(hasSolution);
    m_ui->actNewSolution->setEnabled(true);
    m_ui->actOpenSolution->setEnabled(true);
}

} // namespace nlang
