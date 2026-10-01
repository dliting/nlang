/*--- SolutionModelXml.cpp - .nsln persistence for the project model:
    atomic save, all-or-nothing load (shallow and with-projects), and
    the stream read/write behind both. Node operations live in
    ProjectModel.cpp; .nproj persistence lives in ProjectModelXml.cpp;
    shared plumbing in ProjectXmlIo.h. ---*/
#include "ProjectModel.h"
#include "ProjectPaths.h"
#include "ProjectXmlIo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace nlang {

namespace {

//One <Project> reference entry: path attribute, absolute form against
//solutionDir, duplicate check; the project node is created with its
//file-stem name. False fills error (the caller aborts the load).
bool appendProjectEntry(QXmlStreamReader& xml, const QString& solutionDir,
                        std::vector<QString>& projectPaths,
                        std::vector<std::unique_ptr<ProjectNode>>& projects,
                        QSet<QString>& seenKeys, QString* error) {
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
    return true;
}

//The element walk under <Solution>: depth-aware like the project walk
//in ProjectModelXml.cpp -- only a direct child of <Solution> is a
//<Projects> element, and only a direct child of that is a <Project>
//reference. All-or-nothing staging. False fills error. (Unlike the
//project walk there is no saw-check afterwards: an empty solution is
//legal, so the wrapper flag only feeds the duplicate rejection here.)
bool readProjectReferences(QXmlStreamReader& xml,
                           const QString& solutionDir,
                           std::vector<QString>& projectPaths,
                           std::vector<std::unique_ptr<ProjectNode>>& projects,
                           QString* error) {
    bool inProjects = false;  // currently inside <Projects>
    bool sawProjects = false;  // <Projects> appeared at depth 1
    QSet<QString> seenKeys;
    int open = 1;  // <Solution> root already open; children sit at open==1

    while (!xml.atEnd()) {
        QXmlStreamReader::TokenType token = xml.readNext();

        if (token == QXmlStreamReader::StartElement) {
            if (open == 1 && xml.name() == "Projects") {
                //A second <Projects> block is a schema violation: its
                //entries would be silently dropped -- reject.
                if (sawProjects) {
                    if (error)
                        *error = "solution file has more than one "
                                 "<Projects> element";
                    return false;
                }
                sawProjects = inProjects = true;
            } else if (open == 2 && xml.name() == "Project" && inProjects) {
                if (!appendProjectEntry(xml, solutionDir, projectPaths,
                                        projects, seenKeys, error))
                    return false;
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
    return true;
}

} // namespace

//--- SolutionNode: persistence ---

bool SolutionNode::save(const QString& filePath, QString* error) {
    const QString baseDir = QFileInfo(filePath).absolutePath();

    //Canonical form of every reference against the save target (see
    //ProjectNode::save); collisions fail the save. Computed into locals
    //first: a failed save leaves the node untouched.
    std::vector<QString> absPaths;
    if (!collectCanonicalPaths(m_projectPaths, baseDir,
                               QLatin1String("project"), absPaths, error))
        return false;

    //Project references are written relative to the save target -- Save
    //As relocates the solution directory along with the file.
    if (!writeXmlDocument(
            filePath,
            [&](QXmlStreamWriter& xml) { writeToXml(xml, baseDir, absPaths); },
            error))
        return false;

    //Success: adopt the canonical paths and relocate along with the file.
    //A .nsln now exists on disk, so an ephemeral wrapper is promoted to
    //first-class (the single promotion point -- its changes count as
    //user work from here on).
    m_projectPaths = std::move(absPaths);
    m_solutionDir = baseDir;
    m_ephemeral = false;
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
            *error = QString("cannot open file for reading: %1 (%2)")
                         .arg(filePath, file.errorString());
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
    m_ephemeral = other.m_ephemeral;
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

bool SolutionNode::readFromXml(QXmlStreamReader& xml,
                               const QString& solutionDir, QString* error) {
    if (!readXmlPrologue(xml, QLatin1String("Solution"),
                         QLatin1String("solution"), error))
        return false;

    //Parse into locals; members are committed only after the whole file
    //validated (all-or-nothing). An empty solution (no <Projects>) is
    //legal -- a freshly created solution has no projects yet.
    QXmlStreamAttributes attrs = xml.attributes();
    QString name = attrs.value("name").toString();
    std::vector<QString> projectPaths;
    std::vector<std::unique_ptr<ProjectNode>> projects;

    if (!readProjectReferences(xml, solutionDir, projectPaths, projects,
                               error))
        return false;

    m_name = name;
    m_projectPaths = std::move(projectPaths);
    m_projects = std::move(projects);
    return true;
}

} // namespace nlang
