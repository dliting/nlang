/*--- SearchPathArgs.h - turn project + global library search paths into
    the -I arguments passed to ncc/nvm/ndb. Pure header-only functions
    (no widgets), shared by build/run/debug and their unit tests. ---*/
#ifndef NLANG_TOOLS_NIDE_SEARCH_PATH_ARGS_H
#define NLANG_TOOLS_NIDE_SEARCH_PATH_ARGS_H

#include "ProjectModel.h"
#include "ProjectPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

namespace nlang {

//The anchor for a relative GLOBAL search dir: the user's home dir (global
//settings have no project directory to resolve relative entries against).
inline QString globalPathBase() {
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
}

//Append dir to out unless its normalized key is already present. Returns
//true when appended (first occurrence wins, matching the resolver).
inline bool appendSearchDirOnce(QStringList& out, QSet<QString>& seen,
                                const QString& dir) {
    const QString key = dedupKey(dir);
    if (seen.contains(key))
        return false;
    seen.insert(key);
    out.append(dir);
    return true;
}

//Build the ordered absolute search dirs: project paths first, then global
//paths (project takes precedence). Project paths resolve against
//projectDir; global paths against the user home. Empty/blank entries are
//dropped and the result is de-duplicated on the normalized key.
inline QStringList buildSearchDirs(const QStringList& projectPaths,
                                   const QString& projectDir,
                                   const QStringList& globalPaths) {
    QStringList dirs;
    QSet<QString> seen;

    for (const QString& raw : projectPaths) {
        const QString d = raw.trimmed();
        if (d.isEmpty())
            continue;
        appendSearchDirOnce(dirs, seen, resolvedPath(d, projectDir));
    }
    const QString home = globalPathBase();
    for (const QString& raw : globalPaths) {
        const QString d = raw.trimmed();
        if (d.isEmpty())
            continue;
        appendSearchDirOnce(dirs, seen, resolvedPath(d, home));
    }
    return dirs;
}

//Expand the dirs into interleaved "-I" "<dir>" program arguments.
inline QStringList appendImportArgs(const QStringList& dirs) {
    QStringList args;
    for (const QString& d : dirs) {
        args.append(QStringLiteral("-I"));
        args.append(d);
    }
    return args;
}

//Convenience: build the dirs then expand them to -I arguments in one call.
inline QStringList buildImportArgs(const QStringList& projectPaths,
                                   const QString& projectDir,
                                   const QStringList& globalPaths) {
    return appendImportArgs(
        buildSearchDirs(projectPaths, projectDir, globalPaths));
}

//Convenience: a ProjectNode's configured import dirs in order.
inline QStringList projectImportPathList(const ProjectNode& project) {
    QStringList paths;
    for (int i = 0; i < project.importPathCount(); ++i)
        paths.append(project.importPathAt(i));
    return paths;
}

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_SEARCH_PATH_ARGS_H
