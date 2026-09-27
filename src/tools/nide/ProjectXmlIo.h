/*--- ProjectXmlIo.h - plumbing shared by the .nproj/.nsln persistence
    TUs (ProjectModelXml.cpp / SolutionModelXml.cpp): the XML prologue
    read, the canonical-path collection both savers run, and the atomic
    document write. ---*/
#ifndef NLANG_TOOLS_NIDE_PROJECT_XML_IO_H
#define NLANG_TOOLS_NIDE_PROJECT_XML_IO_H

#include "ProjectPaths.h"

#include <QSaveFile>
#include <QSet>
#include <QString>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <functional>
#include <vector>

namespace nlang {

//Consume the XML declaration and the root start element; the root must
//spell rootName (fileKind names it in the error). False = malformed
//stream or wrong root (the caller aborts the load).
inline bool readXmlPrologue(QXmlStreamReader& xml, QLatin1String rootName,
                            QLatin1String fileKind, QString* error) {
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
    if (xml.name() != rootName) {
        if (error)
            *error = QString("not a %1 file (root element must be <%2>, "
                             "got <%3>)")
                         .arg(fileKind, rootName, xml.name().toString());
        return false;
    }
    return true;
}

//Canonical absolute form of every stored path against baseDir, with the
//duplicate check both savers share ("what gets written must always load
//back"; entryKind names the entry in the error). False fills error and
//leaves absPaths partial -- the caller aborts the save.
inline bool collectCanonicalPaths(const std::vector<QString>& storedPaths,
                                  const QString& baseDir,
                                  QLatin1String entryKind,
                                  std::vector<QString>& absPaths,
                                  QString* error) {
    QSet<QString> keys;
    absPaths.reserve(storedPaths.size());
    for (const QString& stored : storedPaths) {
        const QString abs = resolvedPath(stored, baseDir);
        const QString key = dedupKey(abs);
        if (keys.contains(key)) {
            if (error)
                *error = QString("duplicate %1 entry: %2").arg(entryKind, abs);
            return false;
        }
        keys.insert(key);
        absPaths.push_back(abs);
    }
    return true;
}

//QSaveFile-backed document write: the write goes to a temp file and is
//atomically renamed on commit -- a failed write never truncates the
//existing file. fillDocument receives the configured writer. False
//fills error.
inline bool writeXmlDocument(
        const QString& filePath,
        const std::function<void(QXmlStreamWriter&)>& fillDocument,
        QString* error) {
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
    fillDocument(xml);

    if (xml.hasError() || !file.commit()) {
        if (error)
            *error = QString("write failed: %1").arg(filePath);
        return false;
    }
    return true;
}

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_PROJECT_XML_IO_H
