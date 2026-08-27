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

# Phase 11 stdlib unit tests: in-process compile+run of real .n sources
# via ModuleBuilder (temp .nmod round-trip) + VmExecutor.
add_executable(test_stdlib
    test_vm/test_stdlib.cpp
)

target_include_directories(test_stdlib
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_stdlib
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)

add_test(NAME stdlib_tests COMMAND test_stdlib)
# ScriptScanner location tests (compiler-mode column tracking).
add_executable(test_scanner
    test_compiler/test_scanner.cpp
)

target_link_libraries(test_scanner
    PRIVATE
        nlang_compiler
        nlang_runtime
)

add_test(NAME scanner_tests COMMAND test_scanner)

# AST container model tests (Phase 13 Step 0.5): dual-storage invariant
# for the former out-of-list slots + DetachChild semantics.
add_executable(test_ast_containment
    test_compiler/test_ast_containment.cpp
)

target_link_libraries(test_ast_containment
    PRIVATE
        nlang_compiler
        nlang_runtime
)

add_test(NAME ast_containment_tests COMMAND test_ast_containment)

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

    # Qt Test's plain-text output never reaches stdout on this machine
    # (verified with a minimal official-Qt test app: zero bytes even on
    # a deliberate FAIL), so a failing test gives ctest nothing to show.
    # Every Qt test also writes its full log to qtest_logs/<name>.txt;
    # read that file after any failure.
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qtest_logs)
    function(nlang_add_qtest name target)
        add_test(NAME ${name} COMMAND ${target}
            -o ${CMAKE_CURRENT_BINARY_DIR}/qtest_logs/${name}.txt,txt)
        # House rule: every test stays under a 3-minute ceiling.
        set_tests_properties(${name} PROPERTIES TIMEOUT 180)
    endfunction()
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
    # Qt5::Gui is a hard dependency since SolutionTreeModel joined the
    # library (QStandardItemModel).
    if(WIN32)
        add_custom_command(TARGET test_projectmodel POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
                $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_projectmodel>)
    endif()
    nlang_add_qtest(projectmodel_tests test_projectmodel)

    # IDE solution tree model unit tests (Phase 10 Step 4). The library
    # links Qt5::Gui (QStandardItemModel), which transitively defines
    # QT_GUI_LIB -- a plain QTEST_MAIN would then create a QGuiApplication
    # and demand a platform plugin. QTEST_GUILESS_MAIN pins QCoreApplication.
    add_executable(test_solutiontreemodel
        test_nide/test_solutiontreemodel.cpp
    )
    target_link_libraries(test_solutiontreemodel
        PRIVATE
            nide_projectmodel
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_solutiontreemodel POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
                $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_solutiontreemodel>)
    endif()
    nlang_add_qtest(solutiontreemodel_tests test_solutiontreemodel)

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
    nlang_add_qtest(editor_tests test_editor)
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
    nlang_add_qtest(syntaxhighlighter_tests test_syntaxhighlighter)
    set_tests_properties(syntaxhighlighter_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_PLATFORM_PLUGINS_DIR}")

    # Compile-log browser unit tests (Phase 10 Step 5). Widget test:
    # same QApplication/QPA harness as the editor tests.
    add_executable(test_compilelogbrowser
        test_nide/test_compilelogbrowser.cpp
    )
    target_link_libraries(test_compilelogbrowser
        PRIVATE
            nide_compilelog
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_compilelogbrowser POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
                $<TARGET_FILE:Qt5::Widgets> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_compilelogbrowser>)
    endif()
    nlang_add_qtest(compilelogbrowser_tests test_compilelogbrowser)
    set_tests_properties(compilelogbrowser_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_PLATFORM_PLUGINS_DIR}")

    # Dialog unit tests (Phase 10 Step 6). Widget tests driving modal
    # exec() loops via zero-timeout timers; same QApplication/QPA
    # harness as the editor tests.
    add_executable(test_dialogs
        test_nide/test_dialogs.cpp
    )
    target_link_libraries(test_dialogs
        PRIVATE
            nide_dialogs
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_dialogs POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
                $<TARGET_FILE:Qt5::Widgets> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_dialogs>)
    endif()
    nlang_add_qtest(dialogs_tests test_dialogs)
    set_tests_properties(dialogs_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_PLATFORM_PLUGINS_DIR}")

    # Main-window integration tests (Phase 10 Step 7). These drive the
    # real modal flows (file dialogs, message boxes) and run the real
    # ncc/nvm for build & run, so both exes are copied next to the test
    # binary (MainWindow looks for them in the application directory).
    # The file dialogs need AA_DontUseNativeDialogs (set in the test) to
    # be drivable as QFileDialog widgets. Same QApplication/windows-
    # platform harness as the dialog tests (the offscreen platform has
    # no deployed fonts and crashes in QMessageBox layout).
    add_executable(test_mainwindow
        test_nide/test_mainwindow.cpp
    )
    target_link_libraries(test_mainwindow
        PRIVATE
            nide_mainwindow
            Qt5::Test
    )
    add_dependencies(test_mainwindow ncc nvm)
    if(WIN32)
        add_custom_command(TARGET test_mainwindow POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
                $<TARGET_FILE:Qt5::Widgets> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_mainwindow>)
        add_custom_command(TARGET test_mainwindow POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:ncc> $<TARGET_FILE:nvm>
                $<TARGET_FILE_DIR:test_mainwindow>)
    endif()
    nlang_add_qtest(mainwindow_tests test_mainwindow)
    set_tests_properties(mainwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_PLATFORM_PLUGINS_DIR}")

    # Deployment self-containment check (Phase 10 Step 9). Runs the
    # main-window test suite from a scratch copy of the nide build
    # output with the plugin path cleared (empty env entry: no extra
    # search path) and a qt.conf pinning Qt to the copy -- everything
    # (Qt DLLs, platform plugin, ncc/nvm) must resolve from the copy.
    add_test(NAME nide_deploy_check
        COMMAND ${CMAKE_COMMAND}
            -DDEPLOY_DIR=$<TARGET_FILE_DIR:nide>
            -DTEST_EXE=$<TARGET_FILE:test_mainwindow>
            -DQT_TEST_DLL=$<TARGET_FILE:Qt5::Test>
            -DCHECK_DIR=${CMAKE_CURRENT_BINARY_DIR}/nide_deploy_check
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test_nide/nide_deploy_check.cmake)
    set_tests_properties(nide_deploy_check PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH="
        TIMEOUT 90)
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
