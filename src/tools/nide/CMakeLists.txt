set(CMAKE_AUTOMOC ON)

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
