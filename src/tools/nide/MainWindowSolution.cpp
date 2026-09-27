/*--- MainWindowSolution.cpp - solution and project lifecycle of the
    NLang IDE main window: open/save/close, tree selection, and the
    file-rename pipeline that keeps disk, domain and editors in step. ---*/
#include "MainWindow.h"
#include "BreakpointStore.h"
#include "FileEditor.h"
#include "ProjectModel.h"
#include "ProjectPropDialog.h"
#include "RecentStore.h"
#include "SolutionTreeModel.h"

#include "ui_MainWindow.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

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

} // namespace

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

    FileEditor* editor = m_editors.find(oldPath);
    if (!moveFileOnDisk(oldPath, newPath, trackedFile, editor))
        return false;

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

bool MainWindow::moveFileOnDisk(const QString& oldPath,
                                const QString& newPath,
                                FileNode* trackedFile,
                                FileEditor* editor) {
    //Persist dirty edits to the OLD path first: the disk rename then
    //carries them to the new name, and a failed save aborts with
    //nothing moved.
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

} // namespace nlang
