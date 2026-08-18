add_executable(test_vm
    test_vm/test_vm.cpp
)

target_include_directories(test_vm
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_vm
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)

add_test(NAME vm_tests COMMAND test_vm)

# ncc .nproj loader unit tests (Phase 10 Step 0).
add_executable(test_projectfile
    test_ncc/test_projectfile.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ncc/ProjectFile.cpp
)

target_include_directories(test_projectfile
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/tools/ncc
)

target_link_libraries(test_projectfile
    PRIVATE
        tinyxml2
)

add_test(NAME projectfile_tests COMMAND test_projectfile)

# IDE ProjectModel unit tests (Phase 10 Step 1).
# Requires Qt5 (Core + Test) — only built when IDE is enabled.
if(NLANG_BUILD_IDE)
    find_package(Qt5Test REQUIRED)
    set(CMAKE_AUTOMOC ON)
    add_executable(test_projectmodel
        test_nide/test_projectmodel.cpp
    )
    target_link_libraries(test_projectmodel
        PRIVATE
            nide_projectmodel
            Qt5::Test
    )
    # ctest runs the test exe directly; the Qt runtime DLLs must sit next
    # to it (windeployqt would also work but needs a Qt tools install).
    if(WIN32)
        add_custom_command(TARGET test_projectmodel POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_projectmodel>)
    endif()
    add_test(NAME projectmodel_tests COMMAND test_projectmodel)

    # IDE editor unit tests (Phase 10 Step 2). Widget tests instantiate
    # QApplication, which needs a Qt platform plugin even without showing
    # windows -- point QPA at the Qt install's plugins/platforms.
    find_package(Qt5Widgets REQUIRED)
    add_executable(test_editor
        test_nide/test_editor.cpp
    )
    target_link_libraries(test_editor
        PRIVATE
            nide_editor
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_editor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
                $<TARGET_FILE:Qt5::Widgets> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_editor>)
    endif()
    get_filename_component(QT_PLATFORM_PLUGINS_DIR "${Qt5_DIR}/../../../plugins/platforms" ABSOLUTE)
    add_test(NAME editor_tests COMMAND test_editor)
    set_tests_properties(editor_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_PLATFORM_PLUGINS_DIR}")

    # Syntax highlighter unit tests (Phase 10 Step 3).
    add_executable(test_syntaxhighlighter
        test_nide/test_syntaxhighlighter.cpp
    )
    target_link_libraries(test_syntaxhighlighter
        PRIVATE
            nide_editor
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_syntaxhighlighter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
                $<TARGET_FILE:Qt5::Widgets> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_syntaxhighlighter>)
    endif()
    add_test(NAME syntaxhighlighter_tests COMMAND test_syntaxhighlighter)
    set_tests_properties(syntaxhighlighter_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_PLATFORM_PLUGINS_DIR}")
endif()

# End-to-end test: compile-only (exits 0)
add_test(NAME e2e_compile
    COMMAND ncc build ${PROJECT_SOURCE_DIR}/examples/hello.n)

# Project-mode compile: multi-source .nproj via -p (output redirected into
# the build tree so the source tree stays clean).
add_test(NAME project_compile
    COMMAND ncc build -p ${PROJECT_SOURCE_DIR}/examples/hello_project/hello_project.nproj
    -o ${CMAKE_CURRENT_BINARY_DIR}/hello_project.nmod)

# CLI guardrails: a trailing option without a value and -p combined with a
# positional source must fail with the exact diagnostic (PASS_REGULAR_
# EXPRESSION matches override the nonzero exit — and a crash would not
# match the text).
add_test(NAME ncc_flag_no_value COMMAND ncc build -p)
set_tests_properties(ncc_flag_no_value PROPERTIES
    PASS_REGULAR_EXPRESSION "option -p requires a value")
add_test(NAME ncc_p_conflict COMMAND ncc build -p ${PROJECT_SOURCE_DIR}/examples/hello_project/hello_project.nproj ${PROJECT_SOURCE_DIR}/examples/hello.n)
set_tests_properties(ncc_p_conflict PROPERTIES
    PASS_REGULAR_EXPRESSION "cannot be combined with")

# Project-mode compile+run: exercises the full -p path including loading
# the redirected output. main() maps the cross-file result to 0/1, so a
# clean pass means utils.n's addBoth(40, 2) really returned 42.
add_test(NAME project_run
    COMMAND ncc -p ${PROJECT_SOURCE_DIR}/examples/hello_project/hello_project.nproj
    -o ${CMAKE_CURRENT_BINARY_DIR}/hello_project_run.nmod)
