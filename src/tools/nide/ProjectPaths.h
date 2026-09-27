/*--- ProjectPaths.h - canonical-path helpers shared by the project
    model TUs (in-memory tree + XML persistence) ---*/
#ifndef NLANG_TOOLS_NIDE_PROJECT_PATHS_H
#define NLANG_TOOLS_NIDE_PROJECT_PATHS_H

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace nlang {

//Dedup key for source/project paths: QDir::cleanPath folds "." and ".."
//segments, and on Windows case is folded to match NTFS semantics -- two
//spellings of the same physical file must collide here. Mirrors ncc's
//ProjectFile::SourceKey so both tools accept the same files.
inline QString dedupKey(const QString& path)
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
inline QString resolvedPath(const QString& path, const QString& baseDir)
{
    if (QFileInfo(path).isAbsolute() || baseDir.isEmpty())
        return QDir::cleanPath(path);
    return QDir::cleanPath(QDir(baseDir).absoluteFilePath(path));
}

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_PROJECT_PATHS_H
