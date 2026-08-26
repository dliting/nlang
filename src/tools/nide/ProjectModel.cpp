/*--- ProjectModel.cpp - Solution/Project/File tree model for NLang IDE ---*/
#include "ProjectModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace nlang {

//Dedup key for source/project paths: QDir::cleanPath folds "." and ".."
//segments, and on Windows case is folded to match NTFS semantics -- two
//spellings of the same physical file must collide here. Mirrors ncc's
//ProjectFile::SourceKey so both tools accept the same files.
static QString dedupKey(const QString& path)
{
    QString key = QDir::cleanPath(path);
#ifdef _WIN32
    key = key.toLower();
#endif
    return key;
}

//Canonical stored form: absolute and cleaned. Already-absolute paths are
//only cleaned; relative paths resolve against baseDir. With an empty
//baseDir (project/solution not yet located anywhere) the path is kept
//relative -- writeToXml then interprets it against the save location.
static QString resolvedPath(const QString& path, const QString& baseDir)
{
    if (QFileInfo(path).isAbsolute() || baseDir.isEmpty())
        return QDir::cleanPath(path);
    return QDir::cleanPath(QDir(baseDir).absoluteFilePath(path));
}

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

bool ProjectNode::save(const QString& filePath, QString* error) {
    QString baseDir = QFileInfo(filePath).absolutePath();

    //An empty project would write a file neither this loader nor ncc
    //accepts ("no source files") -- refuse to write it at all.
    if (m_files.empty()) {
        if (error)
            *error = "project has no source files";
        return false;
    }

    //Canonical form of every entry against the save target: entries added
    //before the project had a directory are still relative and become
    //absolute here. Collisions fail the save -- what gets written must
    //always load back. Computed into locals first: a failed save leaves
    //the node untouched.
    std::vector<QString> absPaths;
    absPaths.reserve(m_files.size());
    QSet<QString> keys;
    for (const auto& f : m_files) {
        QString abs = resolvedPath(f->absolutePath(), baseDir);
        QString key = dedupKey(abs);
        if (keys.contains(key)) {
            if (error)
                *error = QString("duplicate source entry: %1").arg(abs);
            return false;
        }
        keys.insert(key);
        absPaths.push_back(abs);
    }

    //QSaveFile: the write goes to a temp file and is atomically renamed on
    //commit -- a failed save never truncates the existing project file.
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QString("cannot open file for writing: %1 (%2)")
                         .arg(filePath, file.errorString());
        return false;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(2);

    //Paths are written relative to the save target -- Save As relocates
    //the project directory along with the file.
    writeToXml(xml, baseDir, absPaths);

    if (xml.hasError() || !file.commit()) {
        if (error)
            *error = QString("write failed: %1").arg(filePath);
        return false;
    }

    //Success: adopt the canonical paths (FileNode identity preserved) and
    //relocate along with the file.
    for (size_t i = 0; i < m_files.size(); ++i)
        m_files[i]->setAbsolutePath(absPaths[i]);
    m_projectDir = baseDir;
    clearDirty();
    return true;
}

bool ProjectNode::load(const QString& filePath, QString* error) {
    QFile file(filePath);
    if (!file.exists()) {
        if (error)
            *error = QString("project file not found: %1").arg(filePath);
        return false;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QString("cannot open file for reading: %1").arg(filePath);
        return false;
    }

    QFileInfo fi(filePath);
    QString dir = fi.absolutePath();

    //readFromXml commits only on full success, so a failed load leaves
    //this node untouched.
    QXmlStreamReader xml(&file);
    if (!readFromXml(xml, dir, error))
        return false;
    m_projectDir = dir;
    //Default name to the full file stem ("my.app.nproj" -> "my.app",
    //same as ncc's std::filesystem::stem()).
    if (m_name.isEmpty())
        m_name = fi.completeBaseName();
    clearDirty();
    return true;
}

void ProjectNode::writeToXml(QXmlStreamWriter& xml, const QString& baseDir,
                              const std::vector<QString>& absPaths) const {
    xml.writeStartDocument();

    xml.writeStartElement("Project");
    xml.writeAttribute("name", m_name);
    if (!m_namespace.isEmpty())
        xml.writeAttribute("namespace", m_namespace);
    if (!m_outputDir.isEmpty())
        xml.writeAttribute("outputDir", m_outputDir);
    if (!m_intermediateDir.isEmpty())
        xml.writeAttribute("intermediateDir", m_intermediateDir);

    xml.writeStartElement("Sources");
    QDir base(baseDir);
    for (const QString& abs : absPaths) {
        xml.writeEmptyElement("File");
        xml.writeAttribute("path", base.relativeFilePath(abs));
    }
    xml.writeEndElement(); // Sources

    xml.writeEndElement(); // Project
    xml.writeEndDocument();
}

bool ProjectNode::readFromXml(QXmlStreamReader& xml, const QString& projectDir,
                              QString* error) {
    // Advance to the first non-whitespace token
    if (xml.readNext() != QXmlStreamReader::StartDocument) {
        if (error)
            *error = "expected XML declaration";
        return false;
    }

    if (xml.readNext() != QXmlStreamReader::StartElement) {
        if (error)
            *error = "expected root element";
        return false;
    }

    if (xml.name() != "Project") {
        if (error)
            *error = QString("not a project file (root element must be <Project>, got <%1>)")
                     .arg(xml.name().toString());
        return false;
    }

    //Parse into locals; members are committed only after the whole file
    //validated (all-or-nothing).
    QXmlStreamAttributes attrs = xml.attributes();
    QString name = attrs.value("name").toString();
    QString ns = attrs.value("namespace").toString();
    QString outputDir = attrs.value("outputDir").toString();
    QString intermediateDir = attrs.value("intermediateDir").toString();
    std::vector<std::unique_ptr<FileNode>> files;

    bool inSources = false;   // currently inside <Sources>
    bool sawSources = false;  // <Sources> appeared at depth 1 (error accuracy)
    QSet<QString> seenKeys;
    int open = 1;  // <Project> root already open; its children sit at open==1

    while (!xml.atEnd()) {
        QXmlStreamReader::TokenType token = xml.readNext();

        if (token == QXmlStreamReader::StartElement) {
            //Depth-aware, matching ncc's FirstChildElement tiers: only a
            //direct child of <Project> is a <Sources> element, and only a
            //direct child of that is a <File> entry.
            if (open == 1 && xml.name() == "Sources") {
                //A second <Sources> block is a schema violation: its files
                //would be silently dropped -- reject (same rule as ncc).
                if (sawSources) {
                    if (error)
                        *error = "project file has more than one <Sources> element";
                    return false;
                }
                sawSources = inSources = true;
            } else if (open == 2 && xml.name() == "File" && inSources) {
                QXmlStreamAttributes fileAttrs = xml.attributes();
                QString path = fileAttrs.value("path").toString();
                if (path.isEmpty()) {
                    if (error)
                        *error = "<File> entry without a path attribute";
                    return false;
                }
                QString absPath = resolvedPath(path, projectDir);
                QString key = dedupKey(absPath);
                if (seenKeys.contains(key)) {
                    if (error)
                        *error = QString("duplicate source entry: %1").arg(absPath);
                    return false;
                }
                seenKeys.insert(key);
                files.push_back(std::make_unique<FileNode>(absPath, this));
            }
            ++open;
        } else if (token == QXmlStreamReader::EndElement) {
            --open;
            //The wrapper flag clears only on the real </Sources> (a
            //direct child of the root lands at open==1); a nested
            //<Sources/> landing deeper must not close it and silently
            //drop the entries after it.
            if (open == 1 && xml.name() == "Sources") {
                inSources = false;
            } else if (open == 0 && xml.name() == "Project") {
                break;  // root closed (a nested <Project/> must not end us)
            }
        } else if (token == QXmlStreamReader::Invalid) {
            if (error)
                *error = QString("XML parse error: %1").arg(xml.errorString());
            return false;
        }
    }

    if (!sawSources) {
        if (error)
            *error = "project file has no <Sources> element";
        return false;
    }

    if (files.empty()) {
        if (error)
            *error = "project has no source files";
        return false;
    }

    m_name = name;
    m_namespace = ns;
    m_outputDir = outputDir;
    m_intermediateDir = intermediateDir;
    m_files = std::move(files);
    return true;
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

bool SolutionNode::save(const QString& filePath, QString* error) {
    QString baseDir = QFileInfo(filePath).absolutePath();

    //Canonical form of every reference against the save target (see
    //ProjectNode::save); collisions fail the save. Computed into locals
    //first: a failed save leaves the node untouched.
    std::vector<QString> absPaths;
    absPaths.reserve(m_projectPaths.size());
    QSet<QString> keys;
    for (const auto& stored : m_projectPaths) {
        QString abs = resolvedPath(stored, baseDir);
        QString key = dedupKey(abs);
        if (keys.contains(key)) {
            if (error)
                *error = QString("duplicate project entry: %1").arg(abs);
            return false;
        }
        keys.insert(key);
        absPaths.push_back(abs);
    }

    //QSaveFile: atomic write -- a failed save never truncates the file.
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QString("cannot open file for writing: %1 (%2)")
                         .arg(filePath, file.errorString());
        return false;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(2);

    //Project references are written relative to the save target -- Save As
    //relocates the solution directory along with the file.
    writeToXml(xml, baseDir, absPaths);

    if (xml.hasError() || !file.commit()) {
        if (error)
            *error = QString("write failed: %1").arg(filePath);
        return false;
    }

    //Success: adopt the canonical paths and relocate along with the file.
    m_projectPaths = std::move(absPaths);
    m_solutionDir = baseDir;
    clearDirty();
    return true;
}

bool SolutionNode::load(const QString& filePath, QString* error) {
    QFile file(filePath);
    if (!file.exists()) {
        if (error)
            *error = QString("solution file not found: %1").arg(filePath);
        return false;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QString("cannot open file for reading: %1").arg(filePath);
        return false;
    }

    QFileInfo fi(filePath);
    QString dir = fi.absolutePath();

    //readFromXml commits only on full success, so a failed load leaves
    //this node untouched.
    QXmlStreamReader xml(&file);
    if (!readFromXml(xml, dir, error))
        return false;
    m_solutionDir = dir;
    if (m_name.isEmpty())
        m_name = fi.completeBaseName();
    clearDirty();
    return true;
}

bool SolutionNode::loadWithProjects(const QString& filePath, QString* error) {
    if (error)
        error->clear();

    //Stage the whole graph: a failure deep in a project file must not
    //leave a half-loaded solution behind (all-or-nothing). Braces, or
    //the declaration parses as a function prototype.
    SolutionNode staged{QString()};
    if (!staged.load(filePath, error))
        return false;

    for (int i = 0; i < staged.projectCount(); ++i) {
        if (!staged.projects()[static_cast<size_t>(i)]->load(
                staged.absoluteProjectPath(i), error)) {
            return false;
        }
    }

    adopt(std::move(staged));
    return true;
}

bool SolutionNode::saveWithProjects(const QString& filePath, QString* error) {
    if (error)
        error->clear();

    //Projects first: the .nsln must not reference unsaved projects.
    //Each saves to its current home (see the header contract); only
    //projects without a known location land next to the save target.
    const QString baseDir = QFileInfo(filePath).absolutePath();
    for (int i = 0; i < projectCount(); ++i) {
        const QString stored = absoluteProjectPath(i);
        const QString absPath = QDir::isRelativePath(stored)
                                    ? QDir(baseDir).filePath(stored)
                                    : stored;
        if (!projects()[static_cast<size_t>(i)]->save(absPath, error))
            return false;
    }
    return save(filePath, error);
}

void SolutionNode::adopt(SolutionNode&& other) {
    m_name = std::move(other.m_name);
    m_dirty = other.m_dirty;
    m_solutionDir = std::move(other.m_solutionDir);
    m_projectPaths = std::move(other.m_projectPaths);
    m_projects = std::move(other.m_projects);
}

void SolutionNode::writeToXml(QXmlStreamWriter& xml, const QString& baseDir,
                               const std::vector<QString>& absPaths) const {
    xml.writeStartDocument();

    xml.writeStartElement("Solution");
    xml.writeAttribute("name", m_name);

    xml.writeStartElement("Projects");
    QDir base(baseDir);
    for (const QString& abs : absPaths) {
        xml.writeEmptyElement("Project");
        xml.writeAttribute("path", base.relativeFilePath(abs));
    }
    xml.writeEndElement(); // Projects

    xml.writeEndElement(); // Solution
    xml.writeEndDocument();
}

bool SolutionNode::readFromXml(QXmlStreamReader& xml, const QString& solutionDir,
                               QString* error) {
    if (xml.readNext() != QXmlStreamReader::StartDocument) {
        if (error)
            *error = "expected XML declaration";
        return false;
    }

    if (xml.readNext() != QXmlStreamReader::StartElement) {
        if (error)
            *error = "expected root element";
        return false;
    }

    if (xml.name() != "Solution") {
        if (error)
            *error = QString("not a solution file (root element must be <Solution>, got <%1>)")
                     .arg(xml.name().toString());
        return false;
    }

    //Parse into locals; members are committed only after the whole file
    //validated (all-or-nothing). An empty solution (no <Projects>) is
    //legal -- a freshly created solution has no projects yet.
    QXmlStreamAttributes attrs = xml.attributes();
    QString name = attrs.value("name").toString();
    std::vector<QString> projectPaths;
    std::vector<std::unique_ptr<ProjectNode>> projects;

    bool inProjects = false;
    bool sawProjects = false;
    QSet<QString> seenKeys;
    int open = 1;  // <Solution> root already open; its children sit at open==1

    while (!xml.atEnd()) {
        QXmlStreamReader::TokenType token = xml.readNext();

        if (token == QXmlStreamReader::StartElement) {
            //Depth-aware, matching ncc's tiering: only a direct child of
            //<Solution> is a <Projects> element, and only a direct child
            //of that is a <Project> entry.
            if (open == 1 && xml.name() == "Projects") {
                //A second <Projects> block is a schema violation: its
                //entries would be silently dropped -- reject.
                if (sawProjects) {
                    if (error)
                        *error = "solution file has more than one <Projects> element";
                    return false;
                }
                sawProjects = inProjects = true;
            } else if (open == 2 && xml.name() == "Project" && inProjects) {
                QXmlStreamAttributes projAttrs = xml.attributes();
                QString path = projAttrs.value("path").toString();
                if (path.isEmpty()) {
                    if (error)
                        *error = "<Project> entry without a path attribute";
                    return false;
                }
                QString absProjPath = resolvedPath(path, solutionDir);
                QString key = dedupKey(absProjPath);
                if (seenKeys.contains(key)) {
                    if (error)
                        *error = QString("duplicate project entry: %1").arg(absProjPath);
                    return false;
                }
                seenKeys.insert(key);
                QFileInfo fi(absProjPath);
                projectPaths.push_back(absProjPath);
                projects.push_back(std::make_unique<ProjectNode>(fi.completeBaseName(),
                                                                 fi.absolutePath()));
            }
            ++open;
        } else if (token == QXmlStreamReader::EndElement) {
            --open;
            //Clears only on the real </Projects> (lands at open==1); a
            //nested <Projects/> must not drop the entries after it.
            if (open == 1 && xml.name() == "Projects") {
                inProjects = false;
            } else if (open == 0 && xml.name() == "Solution") {
                break;  // root closed (a nested <Solution/> must not end us)
            }
        } else if (token == QXmlStreamReader::Invalid) {
            if (error)
                *error = QString("XML parse error: %1").arg(xml.errorString());
            return false;
        }
    }

    m_name = name;
    m_projectPaths = std::move(projectPaths);
    m_projects = std::move(projects);
    return true;
}

} // namespace nlang
