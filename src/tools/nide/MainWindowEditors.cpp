/*--- MainWindowEditors.cpp - file/editor management of the NLang IDE
    main window: open/save/close tabs, the project file menu, the rename
    pipeline and the standalone-file group. ---*/
#include "MainWindow.h"
#include "BreakpointStore.h"
#include "CodeEditor.h"
#include "FileEditor.h"
#include "NewFileDialog.h"
#include "ProjectModel.h"
#include "ProjectPropDialog.h"
#include "RecentStore.h"
#include "SolutionTreeModel.h"

#include "ui_MainWindow.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTabBar>

namespace nlang {

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
    if (!m_solutionTree->hasSolution())
        return;
    //Same routing as the close prompt (saveUnsavedChanges): an ephemeral
    //wrapper has no file of its own, so Save All writes only the dirty
    //projects to their homes -- never a .nsln name dialog for scaffolding.
    if (m_solutionTree->solutionNode()->hasUnsavedChanges())
        saveUnsavedChanges();
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

} // namespace nlang
