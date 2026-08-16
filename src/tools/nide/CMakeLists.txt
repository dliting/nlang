set(CMAKE_AUTOMOC ON)

# ProjectModel library — can be built and tested independently of the full IDE.
add_library(nide_projectmodel
    ProjectModel.cpp
)

target_link_libraries(nide_projectmodel
    PUBLIC
        Qt5::Core
        Qt5::Xml
)

target_include_directories(nide_projectmodel
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
#         Qt5::Xml
#         Qt5::Widgets
# )
