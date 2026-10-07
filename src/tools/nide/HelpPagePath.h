/*--- HelpPagePath.h - locate a page of the docs site shipped next to the
    nide executable; shared by the Help menu and its unit tests. ---*/
#ifndef NLANG_TOOLS_NIDE_HELP_PAGE_PATH_H
#define NLANG_TOOLS_NIDE_HELP_PAGE_PATH_H

#include "SettingsStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace nlang {

//How far above the executable to search for the docs site. The
//installed layout resolves at hop 1 (bin/../docs/site); the dev nide
//exe (build-ide/src/tools/nide/Release) and the test exes
//(<build>/tests/Release) sit deeper -- cap covers both.
inline constexpr int MAX_DOC_SITE_HOPS = 6;

//First ancestor dir whose docs/site/<tree> holds the page, where <tree>
//follows the language setting ("zh"/"en") with the other language as
//fallback. The search is tree-major: the primary tree is walked across
//all ancestor hops before the fallback tree starts, so a primary hit at
//hop 5 beats a fallback hit at hop 1. Empty when no tree holds the page
//(installed layout: bin/../docs/site one hop up; dev tree resolves a
//few hops deeper). documentPagePath is relative to docs/site/<tree>
//without the .html suffix. Tests call it directly.
inline QString locateHelpPage(const QString& documentPagePath) {
    //Tree order: the language setting's tree first, the other
    //language as a fallback (translations land per section, and even
    //a fully shipped tree can miss a brand-new page).
    const QString primary = SettingsStore::persisted().helpTree();
    const QStringList trees = primary == LANGUAGE_ZH
        ? QStringList{LANGUAGE_ZH, LANGUAGE_EN}
        : QStringList{LANGUAGE_EN, LANGUAGE_ZH};
    for (const QString& tree : trees) {
        QDir dir = QCoreApplication::applicationDirPath();
        for (int hop = 0; hop < MAX_DOC_SITE_HOPS; ++hop) {
            //use_directory_urls:false output: flat .html files under
            //the tree root (e.g. "zh/language-spec/overview.html").
            const QString candidate = dir.absoluteFilePath(
                QStringLiteral("docs/site/") + tree
                + QLatin1Char('/') + documentPagePath
                + QStringLiteral(".html"));
            if (QFileInfo::exists(candidate))
                return candidate;
            if (!dir.cdUp())
                break;
        }
    }
    return QString();
}

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_HELP_PAGE_PATH_H
