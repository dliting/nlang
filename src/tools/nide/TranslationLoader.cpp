/*--- TranslationLoader.cpp - install the nide .qm catalogs ---*/
#include "TranslationLoader.h"

//Q_INIT_RESOURCE expands to a do/while block that declares an extern
//function at its expansion point, so the wrapper must sit at global
//scope (inside a namespace the extern would declare a nonexistent
//nlang::qInitResources_*). Without this explicit registration the qrc
//initializer object sits unreferenced inside the static library and
//the linker drops it, leaving :/translations empty.
static void nideInitTranslationsResource() {
    Q_INIT_RESOURCE(nide_translations);
}

namespace nlang {

QTranslator* installTranslations(QApplication* app,
                                 const QLocale& locale) {
    nideInitTranslationsResource();
    auto* translator = new QTranslator(app);
    if (!translator->load(locale, "nide_", QString(),
                          ":/translations")) {
        delete translator;
        return nullptr;
    }
    app->installTranslator(translator);
    return translator;
}

} // namespace nlang
