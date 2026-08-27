/*--- SolutionTreeModel.cpp - Qt item-tree mirroring the solution model ---*/
#include "SolutionTreeModel.h"

#include <QFileInfo>
#include <QIcon>

//Q_INIT_RESOURCE expands to a do/while block declaring an extern
//function at its expansion point, so the wrapper must sit at global
//scope (inside a namespace the extern would declare a nonexistent
//nlang::qInitResources_*). Without this explicit registration the
//static library's qrc initializer object is never linked in and the
//tree icons stay blank.
static void nideInitIconResource() {
    Q_INIT_RESOURCE(nide);
}

namespace nlang {

namespace {

//The three node-kind icons (EN's SolutionModel.cpp:35-36,82).
QIcon nodeIcon(const char* fileName) {
    return QIcon(QString(":/nide/Resources/") + fileName);
}

} // namespace

//--- SolutionTreeItem ---

SolutionTreeItem::SolutionTreeItem(SolutionNode* solution)
    : m_solution(solution), m_project(nullptr), m_file(nullptr)
{
    initText(solution->name());
}

SolutionTreeItem::SolutionTreeItem(ProjectNode* project)
    : m_solution(nullptr), m_project(project), m_file(nullptr)
{
    initText(project->name());
}

SolutionTreeItem::SolutionTreeItem(FileNode* file)
    : m_solution(nullptr), m_project(nullptr), m_file(file)
{
    initText(QFileInfo(file->absolutePath()).fileName());
}

SolutionTreeItem::SolutionTreeItem(NodeType mirrorKind, const QString& text,
                                   const QString& standalonePath)
    : m_solution(nullptr), m_project(nullptr), m_file(nullptr),
      m_standalonePath(mirrorKind == NT_StandaloneFile ? standalonePath
                                                       : QString())
{
    Q_ASSERT(mirrorKind == NT_StandaloneFiles ||
             mirrorKind == NT_StandaloneFile);
    //A pathless NT_StandaloneFile would silently classify (and icon)
    //as the group row in nodeType().
    Q_ASSERT(mirrorKind != NT_StandaloneFile || !standalonePath.isEmpty());
    initText(text);
}

SolutionTreeItem::NodeType SolutionTreeItem::nodeType() const {
    if (m_solution != nullptr)
        return NT_Solution;
    if (m_project != nullptr)
        return NT_Project;
    if (m_file != nullptr)
        return NT_File;
    return m_standalonePath.isEmpty() ? NT_StandaloneFiles
                                      : NT_StandaloneFile;
}

void SolutionTreeItem::initText(const QString& text) {
    setText(text);
    //The tree is a mirror: edits belong to the domain nodes -- except
    //the file row's inline rename, which the model routes through
    //setData -> fileRenameRequested instead of writing the text.
    if (m_file == nullptr)
        setFlags(flags() & ~Qt::ItemIsEditable);
    //Node-kind icon: whichever typed accessor is set. The group row
    //(no pointer, no path) deliberately falls back to solution.png --
    //the resource ships no folder icon.
    setIcon(m_file != nullptr || !m_standalonePath.isEmpty()
                ? nodeIcon("file.png")
                : m_project != nullptr ? nodeIcon("project.png")
                                       : nodeIcon("solution.png"));
}

//--- SolutionTreeModel ---

SolutionTreeModel::SolutionTreeModel(QObject* parent)
    : QStandardItemModel(parent)
{
    //Register :/nide before the first icon pixmap loads (see the
    //wrapper above for why this is explicit).
    nideInitIconResource();
}

SolutionTreeModel::~SolutionTreeModel() = default;

void SolutionTreeModel::newSolution(const QString& name) {
    m_solution = std::make_unique<SolutionNode>(name);
    refresh();
}

bool SolutionTreeModel::loadSolution(const QString& filePath, QString* error) {
    if (error)
        error->clear();

    //Domain-level deep load (all-or-nothing); the model only mirrors.
    //A failed load on a closed model must not leave a phantom solution
    //without a tree (the mirror is only rebuilt on success) -- the
    //closed state stays closed. An open model keeps its solution.
    const bool wasOpen = hasSolution();
    if (!wasOpen)
        m_solution = std::make_unique<SolutionNode>(QString());
    if (!m_solution->loadWithProjects(filePath, error)) {
        if (!wasOpen)
            m_solution.reset();
        return false;
    }

    refresh();
    return true;
}

bool SolutionTreeModel::saveSolution(const QString& filePath, QString* error) {
    if (error)
        error->clear();

    if (!hasSolution()) {
        if (error)
            *error = "no solution is open";
        return false;
    }
    return m_solution->saveWithProjects(filePath, error);
}

void SolutionTreeModel::closeSolution() {
    m_solution.reset();
    refresh();
}

ProjectNode* SolutionTreeModel::addProject(const QString& path) {
    if (!hasSolution())
        return nullptr;

    ProjectNode* project = m_solution->addProject(path);
    if (project == nullptr)
        return nullptr;

    itemAt(index(0, 0))->appendRow(new SolutionTreeItem(project));
    return project;
}

ProjectNode* SolutionTreeModel::openProject(const QString& path,
                                            QString* error) {
    if (error)
        error->clear();
    if (!hasSolution()) {
        if (error)
            *error = "no solution is open";
        return nullptr;
    }
    //addProject/removeProject both mark the SOLUTION dirty; a failed
    //open must not leave that trace behind on a clean solution.
    const bool wasDirty = m_solution->isDirty();
    ProjectNode* project = addProject(path);
    if (project == nullptr) {
        if (error)
            *error = "the solution already contains the project: " + path;
        return nullptr;
    }
    if (!project->load(path, error)) {
        //Roll the half-opened project out of both layers; the caller
        //sees a clean "nothing happened" state.
        removeProject(project);
        if (!wasDirty)
            m_solution->clearDirty();
        return nullptr;
    }
    //The load filled the domain node behind the mirror's back.
    refresh();
    return project;
}

bool SolutionTreeModel::removeProject(ProjectNode* project) {
    if (!hasSolution())
        return false;

    //Locate the row first: the domain remove deletes the node, and a
    //post-delete scan would touch freed memory.
    SolutionTreeItem* root = itemAt(index(0, 0));
    int row = -1;
    for (int r = 0; r < root->rowCount(); ++r) {
        if (root->childItem(r)->project() == project) {
            row = r;
            break;
        }
    }

    if (!m_solution->removeProject(project))
        return false;

    //The mirror tracks the domain; a missing row here means desync.
    Q_ASSERT(row >= 0);
    if (row >= 0)
        root->removeRow(row);
    return true;
}

FileNode* SolutionTreeModel::addFile(ProjectNode* project, const QString& path) {
    //Ownership first: a foreign project must not be mutated before the
    //mirror lookup would fail on it.
    if (!hasSolution() || !ownsProject(project))
        return nullptr;

    FileNode* file = project->addFile(path);
    if (file == nullptr)
        return nullptr;

    itemForProject(project)->appendRow(new SolutionTreeItem(file));
    return file;
}

bool SolutionTreeModel::removeFile(FileNode* file) {
    if (!hasSolution() || file == nullptr)
        return false;

    //Same order discipline as removeProject: the owner pointer and the
    //row must be captured before the domain deletes the node. The owner
    //must belong to this solution (a foreign file is not ours to remove).
    ProjectNode* owner = file->project();
    if (owner == nullptr || !ownsProject(owner))
        return false;

    SolutionTreeItem* projectItem = itemForProject(owner);
    int row = -1;
    for (int r = 0; r < projectItem->rowCount(); ++r) {
        if (projectItem->childItem(r)->file() == file) {
            row = r;
            break;
        }
    }

    if (!owner->removeFile(file))
        return false;

    //The mirror tracks the domain; a missing row here means desync.
    Q_ASSERT(row >= 0);
    if (row >= 0)
        projectItem->removeRow(row);
    return true;
}

bool SolutionTreeModel::renameFile(FileNode* file,
                                   const QString& newAbsolutePath,
                                   QString* error) {
    //Ownership first, like removeFile: a foreign file is not ours.
    if (!hasSolution() || file == nullptr ||
        file->project() == nullptr || !ownsProject(file->project()))
        return false;

    //The domain verdict decides; on failure neither layer moved.
    if (!file->project()->renameFile(file, newAbsolutePath, error))
        return false;

    //Incremental mirror move (no rebuild): item->setText only emits the
    //regular dataChanged, so callers inside a view's edit commit stay
    //safe, and all QModelIndexes remain valid.
    itemForFile(file)->setText(
        QFileInfo(file->absolutePath()).fileName());
    return true;
}

bool SolutionTreeModel::setData(const QModelIndex& index,
                                const QVariant& value, int role) {
    if (role == Qt::EditRole) {
        //An inline edit is a rename request, never a mirror write: the
        //file row hands the new name to the owner via the signal; a
        //rejected rename therefore needs no rollback. The other rows
        //swallow the edit to protect the mirror invariant.
        SolutionTreeItem* item = itemAt(index);
        if (item != nullptr && item->nodeType() == SolutionTreeItem::NT_File) {
            const QString newName = value.toString().trimmed();
            if (newName != item->text())
                emit fileRenameRequested(item->file(), newName);
        }
        return true;
    }
    return QStandardItemModel::setData(index, value, role);
}

SolutionTreeItem* SolutionTreeModel::itemAt(const QModelIndex& index) const {
    return dynamic_cast<SolutionTreeItem*>(itemFromIndex(index));
}

void SolutionTreeModel::refresh() {
    clear();

    if (!hasSolution()) {
        if (!m_standaloneFiles.isEmpty())
            buildStandaloneGroup(nullptr);
        return;
    }

    SolutionTreeItem* root = new SolutionTreeItem(m_solution.get());
    appendRow(root);

    for (const std::unique_ptr<ProjectNode>& project : m_solution->projects()) {
        SolutionTreeItem* projectItem = new SolutionTreeItem(project.get());
        root->appendRow(projectItem);

        for (const std::unique_ptr<FileNode>& file : project->files())
            projectItem->appendRow(new SolutionTreeItem(file.get()));
    }

    if (!m_standaloneFiles.isEmpty())
        buildStandaloneGroup(root);
}

bool SolutionTreeModel::setStandaloneFiles(const QStringList& paths) {
    if (m_standaloneFiles == paths)
        return false;
    m_standaloneFiles = paths;

    if (paths.isEmpty()) {
        //The group's row dies with its last file -- an empty group
        //would be a dangling header.
        if (SolutionTreeItem* group = standaloneGroupItem()) {
            if (QStandardItem* parentItem = group->parent())
                parentItem->removeRow(group->row());
            else
                removeRow(group->row());
        }
        return true;
    }

    if (SolutionTreeItem* group = standaloneGroupItem()) {
        //Incremental sync (the addFile/removeFile discipline): no tree
        //rebuild -- indexes outside the group stay valid (the group's
        //own rows are rebuilt wholesale).
        group->removeRows(0, group->rowCount());
        for (const QString& path : paths)
            group->appendRow(new SolutionTreeItem(
                SolutionTreeItem::NT_StandaloneFile,
                QFileInfo(path).fileName(), path));
    } else {
        buildStandaloneGroup(hasSolution() ? itemAt(index(0, 0))
                                           : nullptr);
    }
    return true;
}

void SolutionTreeModel::buildStandaloneGroup(SolutionTreeItem* parent) {
    SolutionTreeItem* group = new SolutionTreeItem(
        SolutionTreeItem::NT_StandaloneFiles, tr("Standalone Files"));
    if (parent != nullptr)
        parent->appendRow(group);
    else
        appendRow(group);
    for (const QString& path : m_standaloneFiles)
        group->appendRow(new SolutionTreeItem(
            SolutionTreeItem::NT_StandaloneFile,
            QFileInfo(path).fileName(), path));
}

SolutionTreeItem* SolutionTreeModel::standaloneGroupItem() const {
    if (hasSolution()) {
        SolutionTreeItem* root = itemAt(index(0, 0));
        for (int r = 0; r < root->rowCount(); ++r)
            if (root->childItem(r)->nodeType() ==
                SolutionTreeItem::NT_StandaloneFiles)
                return root->childItem(r);
    } else {
        for (int r = 0; r < rowCount(); ++r)
            if (itemAt(index(r, 0))->nodeType() ==
                SolutionTreeItem::NT_StandaloneFiles)
                return itemAt(index(r, 0));
    }
    return nullptr;
}

//True when the project is owned by this solution (a null or foreign
//project must be rejected before any mutation touches it).
bool SolutionTreeModel::ownsProject(const ProjectNode* project) const {
    if (project == nullptr)
        return false;
    for (const std::unique_ptr<ProjectNode>& owned : m_solution->projects()) {
        if (owned.get() == project)
            return true;
    }
    return false;
}

//The project's tree item (scan of the tiny tree beats a back-pointer
//that could go stale across refresh()).
SolutionTreeItem* SolutionTreeModel::itemForProject(ProjectNode* project) const {
    SolutionTreeItem* root = itemAt(index(0, 0));
    for (int r = 0; r < root->rowCount(); ++r) {
        SolutionTreeItem* item = root->childItem(r);
        if (item->project() == project)
            return item;
    }
    return nullptr;  // unreachable while domain and mirror stay in lockstep
}

//The file's tree item; same tiny-tree scan, one level deeper.
SolutionTreeItem* SolutionTreeModel::itemForFile(FileNode* file) const {
    SolutionTreeItem* projectItem = itemForProject(file->project());
    Q_ASSERT(projectItem != nullptr);
    for (int r = 0; r < projectItem->rowCount(); ++r) {
        SolutionTreeItem* item = projectItem->childItem(r);
        if (item->file() == file)
            return item;
    }
    return nullptr;  // unreachable while domain and mirror stay in lockstep
}

} // namespace nlang
