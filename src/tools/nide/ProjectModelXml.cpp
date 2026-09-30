/*--- ProjectModelXml.cpp - .nproj persistence for the project model:
    atomic save, all-or-nothing load, and the stream read/write behind
    both. Node operations live in ProjectModel.cpp; .nsln persistence
    lives in SolutionModelXml.cpp; shared plumbing in ProjectXmlIo.h. ---*/
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

//One <File> entry: path attribute, absolute form against projectDir,
//duplicate check. False fills error (the caller aborts the load).
bool appendSourceEntry(QXmlStreamReader& xml, const QString& projectDir,
                       ProjectNode* owner,
                       std::vector<std::unique_ptr<FileNode>>& files,
                       QSet<QString>& seenKeys, QString* error) {
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
    files.push_back(std::make_unique<FileNode>(absPath, owner));
    return true;
}

//One <Dir> entry under <ImportPaths>: path resolved against projectDir,
//duplicate-checked (same key rules as sources). False fills error.
bool appendImportDirEntry(QXmlStreamReader& xml,
                          const QString& projectDir,
                          std::vector<QString>& importPaths,
                          QSet<QString>& seenKeys, QString* error) {
    const QString path = xml.attributes().value("path").toString();
    if (path.isEmpty()) {
        if (error)
            *error = "<Dir> entry without a path attribute";
        return false;
    }
    const QString absPath = resolvedPath(path, projectDir);
    const QString key = dedupKey(absPath);
    if (seenKeys.contains(key)) {
        if (error)
            *error = QString("duplicate import path: %1").arg(absPath);
        return false;
    }
    seenKeys.insert(key);
    importPaths.push_back(absPath);
    return true;
}

//Walk state under <Project>: wrapper flags, one-block markers, the open
//element depth (root open means children sit at open==1) and seen keys.
struct ProjectWalkState {
    bool inSources = false;
    bool inImportPaths = false;
    bool sawSources = false;
    bool sawImportPaths = false;
    int open = 1;
    QSet<QString> seenSourceKeys;
    QSet<QString> seenImportKeys;
};

//Handle one StartElement (a wrapper open or an entry), advancing open.
//False fills error (duplicate wrapper or a bad entry).
bool handleProjectStart(QXmlStreamReader& xml, const QString& projectDir,
                        ProjectNode* owner,
                        std::vector<std::unique_ptr<FileNode>>& files,
                        std::vector<QString>& importPaths,
                        ProjectWalkState& st, QString* error) {
    const int depth = st.open;
    if (depth == 1 && xml.name() == "Sources") {
        //A second wrapper would silently drop its entries -- reject.
        if (st.sawSources) {
            if (error)
                *error = "project file has more than one <Sources> element";
            return false;
        }
        st.sawSources = st.inSources = true;
    } else if (depth == 1 && xml.name() == "ImportPaths") {
        if (st.sawImportPaths) {
            if (error)
                *error = "project file has more than one <ImportPaths> element";
            return false;
        }
        st.sawImportPaths = st.inImportPaths = true;
    } else if (depth == 2 && xml.name() == "File" && st.inSources) {
        if (!appendSourceEntry(xml, projectDir, owner, files,
                               st.seenSourceKeys, error))
            return false;
    } else if (depth == 2 && xml.name() == "Dir" && st.inImportPaths) {
        if (!appendImportDirEntry(xml, projectDir, importPaths,
                                  st.seenImportKeys, error))
            return false;
    }
    ++st.open;
    return true;
}

//Handle one EndElement: clear wrappers on their real closing tag (a nested
//element must not close one) and detect the root close. False when the
//<Project> root just closed (a nested <Project/> must not end the walk).
bool handleProjectEnd(QXmlStreamReader& xml, ProjectWalkState& st) {
    --st.open;
    if (st.open == 1 && xml.name() == "Sources")
        st.inSources = false;
    else if (st.open == 1 && xml.name() == "ImportPaths")
        st.inImportPaths = false;
    else if (st.open == 0 && xml.name() == "Project")
        return false;
    return true;
}

//The element walk under <Project>: depth-aware, matching ncc's
//FirstChildElement tiers -- only a direct child of <Project> is a wrapper,
//and only a direct child of that is an entry. All-or-nothing: parse stages
//into the out-params/state, committed by the caller on full success.
bool readSourceEntries(QXmlStreamReader& xml, const QString& projectDir,
                       ProjectNode* owner,
                       std::vector<std::unique_ptr<FileNode>>& files,
                       bool* sawSources,
                       std::vector<QString>& importPaths,
                       bool* sawImportPaths, QString* error) {
    ProjectWalkState st;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (!handleProjectStart(xml, projectDir, owner, files,
                                    importPaths, st, error))
                return false;
        } else if (token == QXmlStreamReader::EndElement) {
            if (!handleProjectEnd(xml, st))
                break;
        } else if (token == QXmlStreamReader::Invalid) {
            if (error)
                *error = QString("XML parse error: %1").arg(xml.errorString());
            return false;
        }
    }
    *sawSources = st.sawSources;
    *sawImportPaths = st.sawImportPaths;
    return true;
}

} // namespace

//--- ProjectNode: persistence ---

bool ProjectNode::save(const QString& filePath, QString* error) {
    const QString baseDir = QFileInfo(filePath).absolutePath();

    //An empty project would write a file neither this loader nor ncc
    //accepts ("no source files") -- refuse to write it at all.
    if (m_files.empty()) {
        if (error)
            *error = "project has no source files";
        return false;
    }

    std::vector<QString> storedPaths;
    storedPaths.reserve(m_files.size());
    for (const auto& f : m_files)
        storedPaths.push_back(f->absolutePath());

    //Canonical form of every entry against the save target: entries
    //added before the project had a directory are still relative and
    //become absolute here; collisions fail the save. Computed into
    //locals first: a failed save leaves the node untouched.
    std::vector<QString> absPaths;
    if (!collectCanonicalPaths(storedPaths, baseDir, QLatin1String("source"),
                               absPaths, error))
        return false;

    //Paths are written relative to the save target -- Save As relocates
    //the project directory along with the file.
    if (!writeXmlDocument(
            filePath,
            [&](QXmlStreamWriter& xml) { writeToXml(xml, baseDir, absPaths); },
            error))
        return false;

    //Success: adopt the canonical paths (FileNode identity preserved)
    //and relocate along with the file.
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

    //Library search dirs are optional and written relative to the project
    //file (Save As relocates them along with it).
    if (!m_importPaths.empty()) {
        xml.writeStartElement("ImportPaths");
        for (const QString& dir : m_importPaths) {
            xml.writeEmptyElement("Dir");
            xml.writeAttribute("path", base.relativeFilePath(dir));
        }
        xml.writeEndElement(); // ImportPaths
    }

    xml.writeEndElement(); // Project
    xml.writeEndDocument();
}

bool ProjectNode::readFromXml(QXmlStreamReader& xml, const QString& projectDir,
                              QString* error) {
    if (!readXmlPrologue(xml, QLatin1String("Project"),
                         QLatin1String("project"), error))
        return false;

    //Parse into locals; members are committed only after the whole file
    //validated (all-or-nothing).
    QXmlStreamAttributes attrs = xml.attributes();
    QString name = attrs.value("name").toString();
    QString outputDir = attrs.value("outputDir").toString();
    QString intermediateDir = attrs.value("intermediateDir").toString();
    std::vector<std::unique_ptr<FileNode>> files;
    std::vector<QString> importPaths;
    bool sawSources = false;
    bool sawImportPaths = false;

    if (!readSourceEntries(xml, projectDir, this, files, &sawSources,
                           importPaths, &sawImportPaths, error))
        return false;

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
    m_outputDir = outputDir;
    m_intermediateDir = intermediateDir;
    m_files = std::move(files);
    m_importPaths = std::move(importPaths);
    return true;
}

} // namespace nlang
