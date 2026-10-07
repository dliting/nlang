/*--- WindowLayout.h - splitter-layout persistence of the nide main window:
    the editor-favoring default proportions, the QSettings round trip and
    the restore fallback. Header-only functions acting on any QWidget
    through findChild (they never touch MainWindow state), shared by the
    main window and its unit tests. ---*/
#ifndef NLANG_TOOLS_NIDE_WINDOW_LAYOUT_H
#define NLANG_TOOLS_NIDE_WINDOW_LAYOUT_H

#include <QSettings>
#include <QSplitter>
#include <QWidget>

namespace nlang {

//Default splitter proportions. QSplitter::setSizes reads them as
//RELATIVE shares, so these are not pixels: the solution column keeps a
//narrow fifth, the output pane a quarter of the vertical space.
inline constexpr int DEFAULT_SOLUTION_TREE_SHARE = 20;
inline constexpr int DEFAULT_EDITOR_SHARE = 80;
inline constexpr int DEFAULT_CODE_SHARE = 75;
inline constexpr int DEFAULT_OUTPUT_SHARE = 25;

//QSettings keys for the two splitter states.
inline const char* const LAYOUT_SOLUTION_SPLITTER_KEY =
    "layout/solutionSplitter";
inline const char* const LAYOUT_EDITOR_SPLITTER_KEY = "layout/editorSplitter";

//The tree column must not grab horizontal space (stretch 0/1); inside
//the right column the editor pane wins (stretch 1/0).
inline void applyDefaultLayout(QWidget& window) {
    QSplitter* solutionSplitter =
        window.findChild<QSplitter*>(QStringLiteral("splitter"));
    if (solutionSplitter != nullptr) {
        solutionSplitter->setStretchFactor(0, 0);
        solutionSplitter->setStretchFactor(1, 1);
        solutionSplitter->setSizes({DEFAULT_SOLUTION_TREE_SHARE,
                                     DEFAULT_EDITOR_SHARE});
    }
    QSplitter* editorSplitter =
        window.findChild<QSplitter*>(QStringLiteral("splitter_2"));
    if (editorSplitter != nullptr) {
        editorSplitter->setStretchFactor(0, 1);
        editorSplitter->setStretchFactor(1, 0);
        editorSplitter->setSizes({DEFAULT_CODE_SHARE,
                                  DEFAULT_OUTPUT_SHARE});
    }
}

//Persist both splitter states (QMainWindow::saveState does NOT cover
//central-widget splitters).
inline void saveLayout(const QWidget& window, QSettings& settings) {
    if (QSplitter* splitter =
            window.findChild<QSplitter*>(QStringLiteral("splitter")))
        settings.setValue(LAYOUT_SOLUTION_SPLITTER_KEY,
                          splitter->saveState());
    if (QSplitter* splitter =
            window.findChild<QSplitter*>(QStringLiteral("splitter_2")))
        settings.setValue(LAYOUT_EDITOR_SPLITTER_KEY,
                          splitter->saveState());
}

//Restore the persisted states; falls back to the default layout and
//returns false on garbage or a collapsed pane (tests inject a temporary
//ini).
inline bool restoreLayout(QWidget& window, QSettings& settings) {
    QSplitter* solutionSplitter =
        window.findChild<QSplitter*>(QStringLiteral("splitter"));
    QSplitter* editorSplitter =
        window.findChild<QSplitter*>(QStringLiteral("splitter_2"));
    if (solutionSplitter == nullptr || editorSplitter == nullptr)
        return false;

    const bool restored =
        solutionSplitter->restoreState(
            settings.value(LAYOUT_SOLUTION_SPLITTER_KEY).toByteArray())
        && editorSplitter->restoreState(
            settings.value(LAYOUT_EDITOR_SPLITTER_KEY).toByteArray());
    //A state with a collapsed pane is as useless as no state at all.
    const bool panesVisible =
        solutionSplitter->sizes().at(0) > 0
        && solutionSplitter->sizes().at(1) > 0
        && editorSplitter->sizes().at(0) > 0
        && editorSplitter->sizes().at(1) > 0;
    if (!restored || !panesVisible) {
        applyDefaultLayout(window);
        return false;
    }
    return true;
}

} // namespace nlang

#endif // NLANG_TOOLS_NIDE_WINDOW_LAYOUT_H
