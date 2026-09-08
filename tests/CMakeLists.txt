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

# ndb debugger unit tests: .nmod v1.9 sourceFile, B.1 imported locals,
# debug hooks, read-only view, DebugSessionController session semantics,
# ndb CLI adapter formats, machine-mode line protocol. DebugSession.cpp,
# MachineFrontEnd.cpp and SourceCache.cpp are compiled into the test
# (the tool's main.cpp is not) so the sessions can be driven with string
# streams.
add_executable(test_debugger
    test_vm/test_debugger.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ndb/DebugSession.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ndb/MachineFrontEnd.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ndb/SourceCache.cpp
)

target_include_directories(test_debugger
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/src/tools/ndb
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_debugger
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)

add_test(NAME debugger_tests COMMAND test_debugger)

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

    # Import syntax unit tests (module import visibility): grammar-level
    # acceptance of the identifier-path / wildcard import forms, driven
    # in-process through ModuleBuilder. Public headers only, so the
    # nlang_compiler PUBLIC include dirs cover the includes.
    add_executable(test_import_parse
        test_compiler/test_import_parse.cpp
    )
    target_link_libraries(test_import_parse
        PRIVATE
            nlang_compiler
            nlang_runtime
            nlang_vm
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_import_parse POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_import_parse>)
    endif()
    nlang_add_qtest(import_parse_tests test_import_parse)

    # Module registry unit tests (module import visibility Task 2):
    # per-TU module path computation relative to the project root, the
    # single-file stem fallback and the reserved segment gate, driven
    # in-process through ModuleBuilder (plus one internal header, see
    # the include dir below).
    add_executable(test_module_import
        test_compiler/test_module_import.cpp
    )
    #ModuleRegistry.h lives in the compiler's internal builder dir and
    #VmExecutor.h/ModuleLoader.h in the vm's src dir (both are opaque in
    #the public headers).
    target_include_directories(test_module_import
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src/compiler
            ${PROJECT_SOURCE_DIR}/src/vm
    )
    target_link_libraries(test_module_import
        PRIVATE
            nlang_compiler
            nlang_runtime
            nlang_vm
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_module_import POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_module_import>)
    endif()
    nlang_add_qtest(module_import_tests test_module_import)

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

    # RecentStore unit tests: pure state, no widgets.
    add_executable(test_recentstore
        test_nide/test_recentstore.cpp
    )
    target_link_libraries(test_recentstore
        PRIVATE
            nide_recent
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_recentstore POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_recentstore>)
    endif()
    nlang_add_qtest(recentstore_tests test_recentstore)

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
            nlang_version  # NLANG_VERSION macro for the About-version test
            Qt5::Test
    )
    add_dependencies(test_mainwindow ncc nvm)
    #The help-page test walks up to <build>/docs/site, so the site must
    #exist first. Guarded: NLANG_BUILD_DOCS=OFF defines no such target,
    #and the root CMakeLists defines it before this directory is added.
    if(TARGET nlang_docs)
        add_dependencies(test_mainwindow nlang_docs)
    endif()
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
        #The embedded help browser spawns its QtWebEngine helper from
        #the test exe's directory (see nide's POST_BUILD note).
        nlang_copy_qtwebengine_runtime(test_mainwindow)
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

# Version single-source guard: each tool's --version must echo the
# repository VERSION file (the configure-time value is passed in), so a
# bumped VERSION that missed a rebuild -- or a hardcoded copy -- fails
# here. A Python script asserts exit code AND exact output (ctest regex
# properties cannot check the exit code).
if(Python3_FOUND)
    add_test(NAME version_ncc
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_version.py
            $<TARGET_FILE:ncc> ncc ${NLANG_VERSION})
    add_test(NAME version_nvm
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_version.py
            $<TARGET_FILE:nvm> nvm ${NLANG_VERSION})
    add_test(NAME version_ndisasm
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_version.py
            $<TARGET_FILE:ndisasm> ndisasm ${NLANG_VERSION})
    add_test(NAME version_ndb
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_version.py
            $<TARGET_FILE:ndb> ndb ${NLANG_VERSION})
    set_tests_properties(version_ncc version_nvm version_ndisasm version_ndb
        PROPERTIES TIMEOUT 60)
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

#--- Module import visibility matrix (module import visibility Task 10) --
#Project-mode fixtures in tests/e2e/proj_*: the e2e runner gates only
#single-file and order.txt layouts, so .nproj projects are ctest-only.
#Success projects compile then run (run DEPENDS compile); failure
#projects pin their rejection text with PASS_REGULAR_EXPRESSION (same
#crash-safe pattern as ncc_flag_no_value above: the match overrides the
#nonzero exit, and a crash would print no diagnostic). Run entries rely
#on the same override, so their fixtures self-check BEFORE printing: a
#wrong value exits 9 with empty stdout and the regex cannot match.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/e2e_out)

#Recursive wildcard + explicit imports mixed; the fixture self-checks
#3/4/5 before printing (one line per imported module).
add_test(NAME proj_import_visibility_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_import_visibility/proj_import_visibility.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_import_visibility.nmod)
add_test(NAME proj_import_visibility_run
    COMMAND nvm ${CMAKE_BINARY_DIR}/e2e_out/proj_import_visibility.nmod)
set_tests_properties(proj_import_visibility_run PROPERTIES
    DEPENDS proj_import_visibility_compile
    PASS_REGULAR_EXPRESSION "3")

#D7: a same-directory function is reachable bare AND qualified; the
#fixture self-checks both forms agree (7) before printing.
add_test(NAME proj_same_dir_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_same_dir/proj_same_dir.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_same_dir.nmod)
add_test(NAME proj_same_dir_run
    COMMAND nvm ${CMAKE_BINARY_DIR}/e2e_out/proj_same_dir.nmod)
set_tests_properties(proj_same_dir_run PROPERTIES
    DEPENDS proj_same_dir_compile
    PASS_REGULAR_EXPRESSION "7")

#Duplicate and overlapping imports (exact repeat + exact/wildcard
#overlap) are idempotent: the project compiles clean.
add_test(NAME proj_dup_import_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_dup_import/proj_dup_import.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_dup_import.nmod)

#D11 executed: the wildcard's union reaches the EXACT root module
#(utils.f, main lives in app/ so D7 cannot mask it) and the recursive
#prefix (utils.helper.help); the fixture self-checks both values before
#printing, so a wrong value exits 9 with empty output.
add_test(NAME proj_wildcard_exact_union_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_wildcard_exact_union/proj_wildcard_exact_union.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_wildcard_exact_union.nmod)
add_test(NAME proj_wildcard_exact_union_run
    COMMAND nvm ${CMAKE_BINARY_DIR}/e2e_out/proj_wildcard_exact_union.nmod)
set_tests_properties(proj_wildcard_exact_union_run PROPERTIES
    DEPENDS proj_wildcard_exact_union_compile
    PASS_REGULAR_EXPRESSION "6")

#Spec section 7 row 1: a known module path this TU did not import.
add_test(NAME proj_not_imported_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_not_imported/proj_not_imported.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_not_imported.nmod)
set_tests_properties(proj_not_imported_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "is not imported")

#Spec section 7: the bare pool never spans directories.
add_test(NAME proj_bare_cross_dir_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_bare_cross_dir/proj_bare_cross_dir.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_bare_cross_dir.nmod)
set_tests_properties(proj_bare_cross_dir_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "is not visible here")

#Spec section 5.2: project path segments may not use builtin namespace
#names (io here).
add_test(NAME proj_reserved_segment_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_reserved_segment/proj_reserved_segment.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_reserved_segment.nmod)
set_tests_properties(proj_reserved_segment_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "collides with a built-in namespace")

#Getting-started FAQ pin: without -o the .nmod lands in the process CWD,
#not beside the source (both halves asserted by the script, since ctest
#regexes cannot check the filesystem).
add_test(NAME ncc_no_o_output_cwd
    COMMAND ${CMAKE_COMMAND}
        -DNCC=$<TARGET_FILE:ncc>
        -DSOURCE=${PROJECT_SOURCE_DIR}/examples/hello.n
        -DCWD_DIR=${CMAKE_BINARY_DIR}/e2e_out/cwd_pin
        -P ${CMAKE_CURRENT_SOURCE_DIR}/ncc_no_o_output_cwd.cmake)

#Project-mode entries are fixture compiles/runs; the FAQ CWD pin runs a
#real compile too. Keep each well under the 3-minute unit-test ceiling.
set_tests_properties(proj_import_visibility_compile proj_import_visibility_run
    proj_same_dir_compile proj_same_dir_run proj_dup_import_compile
    proj_wildcard_exact_union_compile proj_wildcard_exact_union_run
    proj_not_imported_compile proj_bare_cross_dir_compile
    proj_reserved_segment_compile ncc_no_o_output_cwd
    PROPERTIES TIMEOUT 180)

#--- Docs-pipeline unit tests (pytest) ---------------------------------------
#The nlang-docs subproject's pytest suite pins the linkcheck and offline-
#search rules; run it from the repo root exactly like the manual gate in
#the README. The PYTHONPATH here is belt-and-suspenders parity with the
#build invocation — the tests also add the package's src/ to sys.path
#themselves, so the suite works without either.
#pytest is not a mkdocs dependency, so a missing pytest must not fail the
#configure -- but the skip must say so: a silently absent entry is
#indistinguishable from a green one.
if(NLANG_BUILD_DOCS)
    execute_process(
        COMMAND ${NLANG_DOCS_PYTHON} -c "import pytest"
        RESULT_VARIABLE NLANG_DOCS_PYTEST_PROBE
        OUTPUT_QUIET ERROR_QUIET)
    if(NLANG_DOCS_PYTEST_PROBE EQUAL 0)
        add_test(NAME nlang_docs_pytest
            COMMAND ${CMAKE_COMMAND} -E env
                PYTHONPATH=${NLANG_DOCS_DIR}/src
                #Snippet-audit tests compile+run for real; the env hands
                #them this tree's tools (tests skip loudly without them).
                NLANG_NCC=$<TARGET_FILE:ncc>
                NLANG_NVM=$<TARGET_FILE:nvm>
                ${NLANG_DOCS_PYTHON} -m pytest ${NLANG_DOCS_DIR}/tests -q
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(nlang_docs_pytest PROPERTIES TIMEOUT 180)
    else()
        message(STATUS "nlang_docs_pytest skipped: pytest is not importable "
            "via '${NLANG_DOCS_PYTHON}' (pip install pytest to enable)")
    endif()
endif()
