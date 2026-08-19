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
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextCursor>

namespace nlang {

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
    updateMenuState();
}

MainWindow::~MainWindow() {
    //Same teardown as closeEvent (whose prompts already ran or don't
    //apply): drop the editors before the members destroy themselves.
    clearEditors();
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
    //One level deeper than selectProject; same tiny-tree scan.
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
    NewFileDialog dialog(this);
    dialog.init(currentProject());
    QString filePath;
    if (!dialog.getFilePath(filePath))
        return;
    editNewFile(filePath);
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
}

void MainWindow::clearEditors() {
    //Deleting an editor deletes its tab page, which fires tab signals
    //on a half-torn state -- block them for the sweep.
    QSignalBlocker blocker(m_ui->tabCodes);
    m_editors.clear();
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
    if (project != nullptr)
        buildProject(*project);
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
    if (project != nullptr)
        runProject(*project);
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
    if (item != nullptr && item->nodeType() == SolutionTreeItem::NT_File)
        editExistingFile(item->file()->absolutePath());
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
    event->accept();
}

//--- menu state ---

void MainWindow::updateMenuState() {
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
    m_ui->actBuild->setEnabled(hasProject);

    //Run lifecycle: Start needs a project AND an idle process; Stop is
    //live exactly while the process runs.
    const bool running =
        m_executed.state() != QProcess::NotRunning;
    m_ui->actStartRunning->setEnabled(hasProject && !running);
    m_ui->actStopRunning->setEnabled(running);

    const bool hasSolution = m_solutionTree->hasSolution();
    m_ui->actSaveSolution->setEnabled(hasSolution);
    m_ui->actCloseSolution->setEnabled(hasSolution);
    m_ui->actNewSolution->setEnabled(true);
    m_ui->actOpenSolution->setEnabled(true);
}

} // namespace nlang
