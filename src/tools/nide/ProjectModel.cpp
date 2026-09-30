/*--- ProjectModel.cpp - Solution/Project/File tree model for NLang IDE:
    in-memory node operations (add/remove/rename, dirty tracking). XML
    persistence lives in ProjectModelXml.cpp. ---*/
#include "ProjectModel.h"
#include "ProjectPaths.h"

#include <QDir>
#include <QFileInfo>

namespace nlang {

//--- FileNode ---

FileNode::FileNode(const QString& absolutePath, ProjectNode* project)
    : m_absolutePath(absolutePath)
    , m_project(project)
{
}

//--- ProjectNode ---

ProjectNode::ProjectNode(const QString& name, const QString& projectDir)
    : m_name(name)
    , m_projectDir(projectDir)
{
}

void ProjectNode::setName(const QString& name) {
    if (m_name != name) {
        m_name = name;
        markDirty();
    }
}

void ProjectNode::setNamespace(const QString& ns) {
    if (m_namespace != ns) {
        m_namespace = ns;
        markDirty();
    }
}

void ProjectNode::setOutputDir(const QString& dir) {
    if (m_outputDir != dir) {
        m_outputDir = dir;
        markDirty();
    }
}

void ProjectNode::setIntermediateDir(const QString& dir) {
    if (m_intermediateDir != dir) {
        m_intermediateDir = dir;
        markDirty();
    }
}

void ProjectNode::setNoWarn(bool noWarn) {
    if (m_noWarn != noWarn) {
        m_noWarn = noWarn;
        markDirty();
    }
}

FileNode* ProjectNode::addFile(const QString& path) {
    QString abs = resolvedPath(path, m_projectDir);
    //Reject duplicates (matched on the normalized path)
    const QString key = dedupKey(abs);
    for (const auto& f : m_files) {
        if (dedupKey(f->absolutePath()) == key)
            return nullptr;
    }
    auto node = std::make_unique<FileNode>(abs, this);
    FileNode* raw = node.get();
    m_files.push_back(std::move(node));
    markDirty();
    return raw;
}

bool ProjectNode::removeFile(FileNode* file) {
    for (auto it = m_files.begin(); it != m_files.end(); ++it) {
        if (it->get() == file) {
            m_files.erase(it);
            markDirty();
            return true;
        }
    }
    return false;
}

bool ProjectNode::renameFile(FileNode* file, const QString& newAbsolutePath,
                             QString* error) {
    if (error)
        error->clear();
    const QString abs = resolvedPath(newAbsolutePath, m_projectDir);
    const QString key = dedupKey(abs);

    //One pass, two verdicts: ownership of the node, and whether any
    //OTHER entry already occupies the target path.
    bool owned = false;
    bool clash = false;
    for (const auto& f : m_files) {
        if (f.get() == file)
            owned = true;
        else if (dedupKey(f->absolutePath()) == key)
            clash = true;
    }
    if (!owned) {
        if (error)
            *error = "file is not part of the project: " + file->absolutePath();
        return false;
    }
    if (clash) {
        if (error)
            *error = "another file already uses the path: " + abs;
        return false;
    }
    file->setAbsolutePath(abs);
    markDirty();
    return true;
}

QString ProjectNode::absolutePathOf(const QString& relativePath) const {
    QDir dir(m_projectDir);
    return dir.absoluteFilePath(relativePath);
}

//--- SolutionNode ---

SolutionNode::SolutionNode(const QString& name)
    : m_name(name)
{
}

void SolutionNode::setName(const QString& name) {
    if (m_name != name) {
        m_name = name;
        markDirty();
    }
}

ProjectNode* SolutionNode::addProject(const QString& path) {
    //Store the canonical form (absolute once the solution dir is known);
    //duplicates collide on the normalized key.
    QString stored = resolvedPath(path, m_solutionDir);
    QString key = dedupKey(stored);
    for (const auto& existing : m_projectPaths) {
        if (dedupKey(existing) == key)
            return nullptr;
    }

    QFileInfo fi(stored);
    //projectDir is known for absolute paths; relative entries (solution
    // not yet saved anywhere) get it on load.
    auto proj = std::make_unique<ProjectNode>(
        fi.completeBaseName(), fi.isAbsolute() ? fi.absolutePath() : QString());
    ProjectNode* raw = proj.get();
    m_projectPaths.push_back(stored);
    m_projects.push_back(std::move(proj));
    markDirty();
    return raw;
}

bool SolutionNode::removeProject(ProjectNode* project) {
    for (size_t i = 0; i < m_projects.size(); ++i) {
        if (m_projects[i].get() == project) {
            m_projects.erase(m_projects.begin() + static_cast<ptrdiff_t>(i));
            m_projectPaths.erase(m_projectPaths.begin() + static_cast<ptrdiff_t>(i));
            markDirty();
            return true;
        }
    }
    return false;
}

QString SolutionNode::projectPath(int index) const {
    Q_ASSERT(index >= 0 && index < static_cast<int>(m_projectPaths.size()));
    const QString& stored = m_projectPaths[static_cast<size_t>(index)];
    if (!m_solutionDir.isEmpty() && QFileInfo(stored).isAbsolute())
        return QDir(m_solutionDir).relativeFilePath(stored);
    return stored;
}

QString SolutionNode::absoluteProjectPath(int index) const {
    Q_ASSERT(index >= 0 && index < static_cast<int>(m_projectPaths.size()));
    return resolvedPath(m_projectPaths[static_cast<size_t>(index)], m_solutionDir);
}

} // namespace nlang
