set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

# ProjectModel library — can be built and tested independently of the full IDE.
# QXmlStreamReader/Writer live in Qt5::Core; the tree model pulls in
# QStandardItemModel from Qt5::Gui; no Xml module dependency. The tree's
# node icons ride in nide.qrc (registered explicitly from
# SolutionTreeModel.cpp -- static-library qrc objects need it).
add_library(nide_projectmodel
    ProjectModel.cpp
    SolutionTreeModel.cpp
    nide.qrc
)
set_target_properties(nide_projectmodel PROPERTIES AUTORCC ON)

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
# the compiler's lexer. BreakpointStore rides here: pure Core state, but
# its only consumers (MainWindow, the gutter painting) live behind this
# library in the link graph.
add_library(nide_editor
    FileEditor.cpp
    CodeEditor.cpp
    SyntaxHighlighter.cpp
    BreakpointStore.cpp
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

# RecentStore: pure MRU state for File > Recent. Own (tiny) library so
# the GUI-less unit test can link Core only, without Widgets.
add_library(nide_recent
    RecentStore.cpp
)

target_link_libraries(nide_recent
    PUBLIC
        Qt5::Core
)

target_include_directories(nide_recent
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# DebugClient/DebugProtocolCodec: line-protocol codec + session client
# for `ndb --machine`. Own (tiny) library so the GUI-less unit tests can
# link Core only. Like the engine tool it drives, this side deliberately
# links no nlang headers (crash isolation + standalone IDE builds); the
# duplicated codec is pinned closed by identical test vectors on both
# sides.
add_library(nide_debug
    DebugProtocolCodec.cpp
    DebugClient.cpp
)

target_link_libraries(nide_debug
    PUBLIC
        Qt5::Core
)

target_include_directories(nide_debug
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
    HelpBrowser.cpp
    MainStatusBar.cpp
    ui/MainWindow.ui
)

set_target_properties(nide_mainwindow PROPERTIES
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/ui
    AUTORCC ON
)

target_link_libraries(nide_mainwindow
    PUBLIC
        Qt5::Widgets
        Qt5::WebEngineWidgets  # embedded help browser
        nide_projectmodel
        nide_editor
        nide_compilelog
        nide_dialogs
        nide_recent
        nide_debug  # DebugClient: the ndb --machine session driver
        nlang_version   # generated NLANG_VERSION macros (VERSION file)
)

#--- translations ---
#Hand-maintained .ts catalogs (regenerate skeletons with lupdate:
#<qt>/bin/lupdate src/tools/nide -recursive -ts translations/nide_*.ts).
#lrelease compiles them and a generated .qrc embeds the .qm into the
#nide_mainwindow library, so loading needs nothing next to the exe.
find_package(Qt5LinguistTools REQUIRED)
qt5_add_translation(NIDE_QM
    translations/nide_zh.ts
    translations/nide_en.ts
)
#rcc resolves listed files relative to the .qrc itself, so write it
#into the build dir with absolute .qm paths inside. The alias keeps
#the registered name clean (":/translations/nide_zh.qm" -- without it
#the whole absolute path becomes the resource name).
set(NIDE_QRC_ENTRIES "")
foreach(NIDE_QM_FILE ${NIDE_QM})
    get_filename_component(NIDE_QM_NAME "${NIDE_QM_FILE}" NAME)
    string(APPEND NIDE_QRC_ENTRIES
        "  <file alias=\"${NIDE_QM_NAME}\">${NIDE_QM_FILE}</file>\n")
endforeach()
set(NIDE_TRANSLATIONS_QRC "${CMAKE_CURRENT_BINARY_DIR}/nide_translations.qrc")
file(WRITE "${NIDE_TRANSLATIONS_QRC}"
"<!DOCTYPE RCC><RCC version=\"2.0\">
<qresource prefix=\"/translations\">
${NIDE_QRC_ENTRIES}</qresource>
</RCC>
")
#The qrc compiles into the library (AUTORCC); the initializer object
#survives linking because TranslationLoader.cpp references it through
#Q_INIT_RESOURCE (see there -- plain static-library qrc objects get
#dropped by the linker).
add_custom_target(nide_translations_qm DEPENDS ${NIDE_QM})
add_dependencies(nide_mainwindow nide_translations_qm)
target_sources(nide_mainwindow
    PRIVATE
        TranslationLoader.cpp
        "${NIDE_TRANSLATIONS_QRC}")

#--- IDE executable ---
add_executable(nide WIN32
    main.cpp
    nide.rc
)

target_link_libraries(nide
    PRIVATE
        nide_mainwindow
)

#A runnable developer build: the Qt runtime DLLs and the ncc/nvm tools
#must sit next to nide.exe (MainWindow runs them from its own
#directory). Self-containment of this layout is verified by the
#nide_deploy_check test (tests/test_nide/).
add_dependencies(nide ncc nvm ndb)
if(WIN32)
    add_custom_command(TARGET nide POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
            $<TARGET_FILE:Qt5::Widgets>
            $<TARGET_FILE:ncc> $<TARGET_FILE:nvm> $<TARGET_FILE:ndb>
            $<TARGET_FILE_DIR:nide>)
    #The platform plugin (qwindows.dll) sits outside Qt5_DIR's lib
    #tree; walk up to the install prefix and back down into it.
    get_filename_component(NIDE_PLATFORM_PLUGINS_DIR
        "${Qt5_DIR}/../../../plugins/platforms" ABSOLUTE)
    add_custom_command(TARGET nide POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${NIDE_PLATFORM_PLUGINS_DIR}"
            "$<TARGET_FILE_DIR:nide>/platforms")
    #QtWebEngine runtime beside the exe (embedded help browser).
    nlang_copy_qtwebengine_runtime(nide)
endif()
