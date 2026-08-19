/*--- SolutionTreeModel.h - Qt item-tree mirroring the solution model ---*/
#ifndef NLANG_TOOLS_NIDE_SOLUTION_TREE_MODEL_H
#define NLANG_TOOLS_NIDE_SOLUTION_TREE_MODEL_H

#include "ProjectModel.h"

#include <QStandardItemModel>

namespace nlang {

//--- SolutionTreeItem: one row of the solution tree; wraps the domain
//  node it was built from. Exactly one typed node accessor returns
//  non-null -- the one matching nodeType(). Items are not editable:
//  renaming happens through the domain nodes (then refresh()).
class SolutionTreeItem : public QStandardItem {
public:
    enum NodeType {
        NT_Solution,
        NT_Project,
        NT_File
    };

    explicit SolutionTreeItem(SolutionNode* solution);
    explicit SolutionTreeItem(ProjectNode* project);
    explicit SolutionTreeItem(FileNode* file);

    NodeType nodeType() const;

    //Typed child accessor: every row under a SolutionTreeItem is one
    //too (this class builds the whole tree), so the downcast is exact.
    SolutionTreeItem* childItem(int row) const {
        return static_cast<SolutionTreeItem*>(child(row));
    }

    SolutionNode* solution() const { return m_solution; }
    ProjectNode* project() const { return m_project; }
    FileNode* file() const { return m_file; }

private:
    //Display text derives from the node (name, or the file name).
    void initText(const QString& text);

    SolutionNode* m_solution;  // non-null only for NT_Solution
    ProjectNode* m_project;    // non-null only for NT_Project
    FileNode* m_file;          // non-null only for NT_File
};

//--- SolutionTreeModel: owns the SolutionNode and mirrors it as a
//  three-level item tree (Solution -> Project -> File). All mutations
//  go through this class (or the domain node + refresh()); the mirror
//  is never edited directly. Double-click and selection wiring is the
//  view's job (MainWindow, later step) via itemAt().
class SolutionTreeModel : public QStandardItemModel {
    Q_OBJECT
public:
    explicit SolutionTreeModel(QObject* parent = nullptr);
    ~SolutionTreeModel() override;

    //--- solution lifecycle ---
    //Replace any open solution with an empty one (creating a solution).
    void newSolution(const QString& name);

    //Deep load via SolutionNode::loadWithProjects (all-or-nothing); the
    //tree is rebuilt only on success.
    bool loadSolution(const QString& filePath, QString* error = nullptr);

    //Deep save via SolutionNode::saveWithProjects -- see there for the
    //project-location policy (existing projects keep their home).
    bool saveSolution(const QString& filePath, QString* error = nullptr);

    //Drop the solution; the tree becomes empty.
    void closeSolution();

    bool hasSolution() const { return m_solution != nullptr; }

    //Null when no solution is open.
    SolutionNode* solutionNode() const { return m_solution.get(); }

    //--- tree mutations (domain + mirror stay in lockstep) ---
    //Null when the model is closed or the project is a duplicate.
    ProjectNode* addProject(const QString& path);

    //Open a .nproj: add it to the solution AND load its content (the
    //plain addProject leaves the node empty). All-or-nothing -- on a
    //duplicate or a load failure nothing changes; error tells why.
    ProjectNode* openProject(const QString& path, QString* error = nullptr);

    //False when the project is not owned by this solution.
    bool removeProject(ProjectNode* project);

    //Null when the project is unknown or the file is a duplicate.
    FileNode* addFile(ProjectNode* project, const QString& path);

    //False when the file is not owned by this solution.
    bool removeFile(FileNode* file);

    //The tree item at index, or nullptr for invalid/unwrapped rows.
    SolutionTreeItem* itemAt(const QModelIndex& index) const;

    //Rebuild the item tree from the domain node -- after mutations made
    //directly on the nodes (property dialogs etc.).
    void refresh();

private:
    //True when the project is owned by this solution.
    bool ownsProject(const ProjectNode* project) const;

    //The project's tree item; scans the (tiny) tree instead of keeping
    //a node back-pointer that refresh() would invalidate.
    SolutionTreeItem* itemForProject(ProjectNode* project) const;

    std::unique_ptr<SolutionNode> m_solution;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_SOLUTION_TREE_MODEL_H
