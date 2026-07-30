set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

add_executable(nide
    main.cpp
    MainWindow.cpp
    CodeEditor.cpp
    SyntaxHighlighter.cpp
    SolutionModel.cpp
    FileEditor.cpp
    CompileLogBrowser.cpp
    ProjectPropDialog.cpp
    NewFileDiaLog.cpp
    StatusBar.cpp
    ui/MainWindow.ui
    ui/NewFile.ui
    ui/ProjectProp.ui
)

target_link_libraries(nide
    PRIVATE
        nlang_compiler
        nlang_runtime
        Qt5::Core
        Qt5::Xml
        Qt5::Widgets
)
