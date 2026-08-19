set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

# ProjectModel library — can be built and tested independently of the full IDE.
# QXmlStreamReader/Writer live in Qt5::Core; the tree model pulls in
# QStandardItemModel from Qt5::Gui; no Xml module dependency.
add_library(nide_projectmodel
    ProjectModel.cpp
    SolutionTreeModel.cpp
)

target_link_libraries(nide_projectmodel
    PUBLIC
        Qt5::Core
        Qt5::Gui
)

target_include_directories(nide_projectmodel
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Editor library -- FileEditor/CodeEditor widgets + token coloring via
# the compiler's lexer.
add_library(nide_editor
    FileEditor.cpp
    CodeEditor.cpp
    SyntaxHighlighter.cpp
)

target_link_libraries(nide_editor
    PUBLIC
        Qt5::Widgets
        nlang_compiler  # nlang_runtime comes transitively (PUBLIC on it)
)

target_include_directories(nide_editor
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Compile-log browser library -- parses ncc output lines so a double
# click can jump to the error site (path resolution via ProjectNode).
add_library(nide_compilelog
    CompileLogBrowser.cpp
)

target_link_libraries(nide_compilelog
    PUBLIC
        Qt5::Widgets
        nide_projectmodel
)

target_include_directories(nide_compilelog
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Dialogs library -- new-file and project-properties forms. The .ui
# files ride along as target sources so AUTOUIC generates ui_*.h.
# AUTOUIC matches an #include "ui_Foo.h" against Foo.ui in the source
# file's directory by default; ours sit in ui/, so point the search
# there.
add_library(nide_dialogs
    NewFileDialog.cpp
    ProjectPropDialog.cpp
    ui/NewFileDialog.ui
    ui/ProjectPropDialog.ui
)

set_target_properties(nide_dialogs PROPERTIES
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

target_link_libraries(nide_dialogs
    PUBLIC
        Qt5::Widgets
        nide_projectmodel
)

target_include_directories(nide_dialogs
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Main-window library -- the assembled IDE frame: menus, editor tabs,
# solution tree, build & run. The promoted custom widgets in
# MainWindow.ui (MainStatusBar, CompileLogBrowser) are resolved via
# the PUBLIC include dir above; their headers carry global
# using-declarations because uic cannot emit namespaced types.
add_library(nide_mainwindow
    MainWindow.cpp
    MainStatusBar.cpp
    ui/MainWindow.ui
)

set_target_properties(nide_mainwindow PROPERTIES
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

target_link_libraries(nide_mainwindow
    PUBLIC
        Qt5::Widgets
        nide_projectmodel
        nide_editor
        nide_compilelog
        nide_dialogs
)

# Full IDE executable — will be added in later steps as components are migrated.
# add_executable(nide
#     main.cpp
#     MainWindow.cpp
#     CodeEditor.cpp
#     SyntaxHighlighter.cpp
#     SolutionModel.cpp
#     FileEditor.cpp
#     CompileLogBrowser.cpp
#     ProjectPropDialog.cpp
#     NewFileDiaLog.cpp
#     StatusBar.cpp
#     ProjectModel.cpp
#     ui/MainWindow.ui
#     ui/NewFile.ui
#     ui/ProjectProp.ui
# )
#
# target_link_libraries(nide
#     PRIVATE
#         nlang_compiler
#         nlang_runtime
#         nide_projectmodel
#         Qt5::Core
#         Qt5::Widgets
# )
