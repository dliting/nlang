/*--- main.cpp - nide entry point ---*/
#include "MainWindow.h"
#include "TranslationLoader.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    //QtWebEngine (the embedded help browser) requires shared OpenGL
    //contexts; the attribute must be set before QApplication exists.
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    //Layout persistence keys QSettings by organization + application.
    QCoreApplication::setOrganizationName(QStringLiteral("NLang"));
    QCoreApplication::setApplicationName(QStringLiteral("nide"));
    //Catalogs are embedded (qrc); falls back to the authored strings.
    nlang::installTranslations(&app);
    nlang::MainWindow window;
    window.showMaximized();
    return app.exec();
}
