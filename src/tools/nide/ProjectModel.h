/*--- ProjectModel.h - Solution/Project/File tree model for NLang IDE ---*/
#ifndef NLANG_TOOLS_NIDE_PROJECT_MODEL_H
#define NLANG_TOOLS_NIDE_PROJECT_MODEL_H

#include <QString>
#include <memory>
#include <vector>

class QXmlStreamReader;
class QXmlStreamWriter;

namespace nlang {

class ProjectNode;

//--- FileNode: a source file within a project.
//  Stores the absolute path and a back-pointer to the owning project.
//  Paths are resolved to absolute on load; save() writes them relative
//  to the project directory.
class FileNode {
public:
    FileNode(const QString& absolutePath, ProjectNode* project);
    ~FileNode() = default;

    FileNode(const FileNode&) = delete;
    FileNode& operator=(const FileNode&) = delete;

    const QString& absolutePath() const { return m_absolutePath; }
    ProjectNode* project() const { return m_project; }

    //Used by ProjectNode::save to re-base entries that were added before
    //the project had a directory -- identity of the node is preserved.
    void setAbsolutePath(const QString& absolutePath) { m_absolutePath = absolutePath; }

private:
    QString m_absolutePath;
    ProjectNode* m_project;  // non-owning back-pointer
};

//--- ProjectNode: a single .nproj project within a solution.
//  Owns a list of FileNode instances. Supports XML load/save,
//  dirty tracking, and absolute path resolution.
class ProjectNode {
public:
    // Construct with name and project directory (absolute).
    // The directory is where the .nproj file resides.
    ProjectNode(const QString& name, const QString& projectDir);
    ~ProjectNode() = default;

    ProjectNode(const ProjectNode&) = delete;
    ProjectNode& operator=(const ProjectNode&) = delete;

    //--- Properties ---
    const QString& name() const { return m_name; }
    void setName(const QString& name);

    const QString& projectDir() const { return m_projectDir; }

    const QString& namespace_() const { return m_namespace; }
    void setNamespace(const QString& ns);

    const QString& outputDir() const { return m_outputDir; }
    void setOutputDir(const QString& dir);

    const QString& intermediateDir() const { return m_intermediateDir; }
    void setIntermediateDir(const QString& dir);

    //--- Dirty tracking ---
    bool isDirty() const { return m_dirty; }
    void clearDirty() { m_dirty = false; }

    //--- File management ---
    // Adds a file to the project. Relative paths are resolved against the
    // project directory and stored absolute (kept as given while no
    // project directory is known). Returns nullptr if the same file
    // (matched on the normalized path) already exists.
    FileNode* addFile(const QString& path);

    // Removes and deletes a file. Returns false if the file is not owned
    // by this project.
    bool removeFile(FileNode* file);

    int fileCount() const { return static_cast<int>(m_files.size()); }
    const std::vector<std::unique_ptr<FileNode>>& files() const { return m_files; }

    //--- Path resolution ---
    // Resolves a relative path against the project directory.
    QString absolutePathOf(const QString& relativePath) const;

    //--- XML persistence ---
    // Save the project to the given file path (atomically). Source paths
    // are written relative to the file's directory, and the project
    // directory is updated to it -- Save As relocates the project. Fails
    // if two entries collide once canonicalized or the project has no
    // files (the written file must always be loadable).
    bool save(const QString& filePath, QString* error = nullptr);

    // Load the project from the given .nproj file. The projectDir is
    // set to the parent directory of the file. All-or-nothing: on
    // failure the node keeps its previous state.
    bool load(const QString& filePath, QString* error = nullptr);

private:
    void markDirty() { m_dirty = true; }

    // Write the given absolute source paths relative to baseDir.
    void writeToXml(QXmlStreamWriter& xml, const QString& baseDir,
                    const std::vector<QString>& absPaths) const;

    // Read project content from XML stream (the <Project> element is
    // expected to be the current element). Parses into locals and commits
    // only on full success. Returns true on success.
    bool readFromXml(QXmlStreamReader& xml, const QString& projectDir, QString* error);

    QString m_name;
    QString m_projectDir;       // absolute directory of the .nproj file
    QString m_namespace;
    QString m_outputDir;        // relative to projectDir
    QString m_intermediateDir;  // relative to projectDir
    bool m_dirty = false;

    std::vector<std::unique_ptr<FileNode>> m_files;
};

//--- SolutionNode: the root of the solution tree.
//  Owns a list of ProjectNode instances. Supports XML load/save (.nsln),
//  dirty tracking. Project paths in the .nsln are relative to the
//  solution directory.
class SolutionNode {
public:
    explicit SolutionNode(const QString& name);
    ~SolutionNode() = default;

    SolutionNode(const SolutionNode&) = delete;
    SolutionNode& operator=(const SolutionNode&) = delete;

    //--- Properties ---
    const QString& name() const { return m_name; }
    void setName(const QString& name);

    //--- Dirty tracking ---
    bool isDirty() const { return m_dirty; }
    void clearDirty() { m_dirty = false; }

    //--- Project management ---
    // Adds a project by path. Relative paths resolve against the solution
    // directory when known; until the first load/save they are kept as
    // given (relative to the future save location). The project name
    // defaults to the file stem. Returns nullptr on duplicate.
    ProjectNode* addProject(const QString& path);

    // Removes and deletes a project. Returns false if not owned.
    bool removeProject(ProjectNode* project);

    int projectCount() const { return static_cast<int>(m_projects.size()); }
    const std::vector<std::unique_ptr<ProjectNode>>& projects() const { return m_projects; }

    // Returns the path of the i-th project, relative to the solution
    // directory (or as stored if no solution directory is known yet).
    // Index must be in [0, projectCount()).
    QString projectPath(int index) const;

    //--- XML persistence ---
    // Save the solution to the given file path (atomically). Project
    // references are written relative to the file's directory, and the
    // solution directory is updated to it -- Save As relocates the
    // solution. Fails if two references collide once canonicalized.
    bool save(const QString& filePath, QString* error = nullptr);

    // Load the solution from the given .nsln file. All-or-nothing: on
    // failure the node keeps its previous state.
    bool load(const QString& filePath, QString* error = nullptr);

private:
    void markDirty() { m_dirty = true; }

    // Write the given absolute project paths relative to baseDir.
    void writeToXml(QXmlStreamWriter& xml, const QString& baseDir,
                    const std::vector<QString>& absPaths) const;

    // Read solution content from XML stream. Parses into locals and
    // commits only on full success. Returns true on success.
    bool readFromXml(QXmlStreamReader& xml, const QString& solutionDir, QString* error);

    QString m_name;
    bool m_dirty = false;
    QString m_solutionDir;  // absolute directory of the loaded/saved .nsln

    // Parallel arrays: project paths and owned projects. Paths are stored
    // absolute once the solution directory is known (relative until then);
    // writeToXml relativizes against the save location.
    std::vector<QString> m_projectPaths;
    std::vector<std::unique_ptr<ProjectNode>> m_projects;
};

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_PROJECT_MODEL_H
