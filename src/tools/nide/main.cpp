/*--- main.cpp - nide entry point ---*/
#include "MainWindow.h"
#include "TranslationLoader.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    //Catalogs are embedded (qrc); falls back to the authored strings.
    nlang::installTranslations(&app);
    nlang::MainWindow window;
    window.showMaximized();
    return app.exec();
}
