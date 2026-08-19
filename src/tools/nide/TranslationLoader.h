/*--- TranslationLoader.h - install the nide .qm catalogs ---*/
#ifndef NLANG_TOOLS_NIDE_TRANSLATION_LOADER_H
#define NLANG_TOOLS_NIDE_TRANSLATION_LOADER_H

#include <QApplication>
#include <QLocale>
#include <QTranslator>

namespace nlang {

//Load and install the catalog matching locale onto app. Falls back
//through the locale's uiLanguages (zh_CN -> zh); the .qm files are
//embedded in the nide_mainwindow library (qrc :/translations), so no
//files ship next to the executable. Returns the installed translator
//(parented to app; keep it to uninstall) or null when no catalog
//matched -- the UI then shows its authored strings.
QTranslator* installTranslations(QApplication* app,
                                 const QLocale& locale = QLocale());

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_TRANSLATION_LOADER_H
