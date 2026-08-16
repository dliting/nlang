/*--- ProjectModel.cpp - Solution/Project/File tree model for NLang IDE ---*/
#include "ProjectModel.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>

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

FileNode* ProjectNode::addFile(const QString& absolutePath) {
    // Reject duplicates
    for (const auto& f : m_files) {
        if (f->absolutePath() == absolutePath)
            return nullptr;
    }
    auto node = std::make_unique<FileNode>(absolutePath, this);
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

QString ProjectNode::absolutePathOf(const QString& relativePath) const {
    QDir dir(m_projectDir);
    return dir.absoluteFilePath(relativePath);
}

bool ProjectNode::save(const QString& filePath, QString* error) {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QString("cannot open file for writing: %1").arg(filePath);
        return false;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(2);

    writeToXml(xml);

    if (xml.hasError()) {
        if (error)
            *error = QString("XML write error in %1").arg(filePath);
        return false;
    }

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

    QXmlStreamReader xml(&file);
    bool ok = readFromXml(xml, dir, error);
    if (ok) {
        m_projectDir = dir;
        // Default name to file stem if the XML didn't provide one.
        if (m_name.isEmpty())
            m_name = fi.baseName();
        clearDirty();
    }
    return ok;
}

void ProjectNode::writeToXml(QXmlStreamWriter& xml) const {
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
    QDir projDir(m_projectDir);
    for (const auto& f : m_files) {
        // Write path relative to the project directory
        QString rel = projDir.relativeFilePath(f->absolutePath());
        xml.writeEmptyElement("File");
        xml.writeAttribute("path", rel);
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

    // Read attributes
    QXmlStreamAttributes attrs = xml.attributes();
    m_name = attrs.value("name").toString();
    m_namespace = attrs.value("namespace").toString();
    m_outputDir = attrs.value("outputDir").toString();
    m_intermediateDir = attrs.value("intermediateDir").toString();

    // Read child elements
    bool foundSources = false;
    QSet<QString> seenPaths;

    while (!xml.atEnd()) {
        QXmlStreamReader::TokenType token = xml.readNext();

        if (token == QXmlStreamReader::StartElement) {
            if (xml.name() == "Sources") {
                foundSources = true;
            } else if (xml.name() == "File" && foundSources) {
                QXmlStreamAttributes fileAttrs = xml.attributes();
                QString path = fileAttrs.value("path").toString();
                if (path.isEmpty()) {
                    if (error)
                        *error = "<File> entry without a path attribute";
                    return false;
                }
                // Resolve to absolute
                QDir dir(projectDir);
                QString absPath = dir.absoluteFilePath(path);
                if (seenPaths.contains(absPath)) {
                    if (error)
                        *error = QString("duplicate source entry: %1").arg(absPath);
                    return false;
                }
                seenPaths.insert(absPath);
                m_files.push_back(std::make_unique<FileNode>(absPath, this));
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (xml.name() == "Sources") {
                foundSources = false;
            } else if (xml.name() == "Project") {
                break;
            }
        } else if (token == QXmlStreamReader::Invalid) {
            if (error)
                *error = QString("XML parse error: %1").arg(xml.errorString());
            return false;
        }
    }

    if (!foundSources && m_files.empty()) {
        // We never entered a <Sources> element at all
        if (error)
            *error = "project file has no <Sources> element";
        return false;
    }

    if (m_files.empty()) {
        if (error)
            *error = "project has no source files";
        return false;
    }

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

ProjectNode* SolutionNode::addProject(const QString& relativePath) {
    // Check for duplicate path
    for (const auto& path : m_projectPaths) {
        if (path == relativePath)
            return nullptr;
    }

    QString name = QFileInfo(relativePath).baseName();
    // projectDir will be resolved on load; for now use empty string
    // since we don't know the solution directory yet.
    auto proj = std::make_unique<ProjectNode>(name, QString());
    ProjectNode* raw = proj.get();
    m_projectPaths.push_back(relativePath);
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

const QString& SolutionNode::projectPath(int index) const {
    Q_ASSERT(index >= 0 && index < static_cast<int>(m_projectPaths.size()));
    return m_projectPaths[static_cast<size_t>(index)];
}

bool SolutionNode::save(const QString& filePath, QString* error) {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QString("cannot open file for writing: %1").arg(filePath);
        return false;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(2);

    writeToXml(xml);

    if (xml.hasError()) {
        if (error)
            *error = QString("XML write error in %1").arg(filePath);
        return false;
    }

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

    QXmlStreamReader xml(&file);
    bool ok = readFromXml(xml, dir, error);
    if (ok)
        clearDirty();
    return ok;
}

void SolutionNode::writeToXml(QXmlStreamWriter& xml) const {
    xml.writeStartDocument();

    xml.writeStartElement("Solution");
    xml.writeAttribute("name", m_name);

    xml.writeStartElement("Projects");
    for (size_t i = 0; i < m_projectPaths.size(); ++i) {
        xml.writeEmptyElement("Project");
        xml.writeAttribute("path", m_projectPaths[i]);
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

    QXmlStreamAttributes attrs = xml.attributes();
    m_name = attrs.value("name").toString();

    bool foundProjects = false;

    while (!xml.atEnd()) {
        QXmlStreamReader::TokenType token = xml.readNext();

        if (token == QXmlStreamReader::StartElement) {
            if (xml.name() == "Projects") {
                foundProjects = true;
            } else if (xml.name() == "Project" && foundProjects) {
                QXmlStreamAttributes projAttrs = xml.attributes();
                QString path = projAttrs.value("path").toString();
                if (path.isEmpty()) {
                    if (error)
                        *error = "<Project> entry without a path attribute";
                    return false;
                }
                // Resolve the project directory from the solution dir
                QDir solDir(solutionDir);
                QString absProjPath = solDir.absoluteFilePath(path);
                QString projDir = QFileInfo(absProjPath).absolutePath();
                QString name = QFileInfo(absProjPath).baseName();

                m_projectPaths.push_back(path);
                m_projects.push_back(std::make_unique<ProjectNode>(name, projDir));
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (xml.name() == "Projects") {
                foundProjects = false;
            } else if (xml.name() == "Solution") {
                break;
            }
        } else if (token == QXmlStreamReader::Invalid) {
            if (error)
                *error = QString("XML parse error: %1").arg(xml.errorString());
            return false;
        }
    }

    return true;
}

} // namespace nlang
