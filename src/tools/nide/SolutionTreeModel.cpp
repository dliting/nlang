/*--- SolutionTreeModel.cpp - Qt item-tree mirroring the solution model ---*/
#include "SolutionTreeModel.h"

#include <QFileInfo>

namespace nlang {

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

SolutionTreeItem::NodeType SolutionTreeItem::nodeType() const {
    if (m_solution != nullptr)
        return NT_Solution;
    if (m_project != nullptr)
        return NT_Project;
    return NT_File;
}

void SolutionTreeItem::initText(const QString& text) {
    setText(text);
    //The tree is a mirror: edits belong to the domain nodes.
    setFlags(flags() & ~Qt::ItemIsEditable);
}

//--- SolutionTreeModel ---

SolutionTreeModel::SolutionTreeModel(QObject* parent)
    : QStandardItemModel(parent)
{
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

SolutionTreeItem* SolutionTreeModel::itemAt(const QModelIndex& index) const {
    return dynamic_cast<SolutionTreeItem*>(itemFromIndex(index));
}

void SolutionTreeModel::refresh() {
    clear();

    if (!hasSolution())
        return;

    SolutionTreeItem* root = new SolutionTreeItem(m_solution.get());
    appendRow(root);

    for (const std::unique_ptr<ProjectNode>& project : m_solution->projects()) {
        SolutionTreeItem* projectItem = new SolutionTreeItem(project.get());
        root->appendRow(projectItem);

        for (const std::unique_ptr<FileNode>& file : project->files())
            projectItem->appendRow(new SolutionTreeItem(file.get()));
    }
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

} // namespace nlang
