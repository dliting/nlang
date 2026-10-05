# Stdlib dir every STDLIB_DIR-defining test compiles and loads against:
# the BUILD-TREE copy (root CMakeLists "Standard library package") — the
# same dir the tools resolve via FindStdLibDir. It carries the library
# sources for compile-time signature resolution AND stdlib.npkg for the
# load-time closure (NcuLoader search dirs), so tests resolve stdlib
# exactly like production instead of half source-tree, half nowhere.
# The dependency block at the end of this file packs the package (and
# refreshes the copy) before any of these targets build, so targeted
# test builds work on a fresh build tree too.
set(NLANG_TEST_STDLIB_DIR ${CMAKE_BINARY_DIR}/stdlib)

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

# nlink unit tests: synthetic per-unit module images drive the load-time
# linker (peer merge, slot resolution, operand remap) — no codegen, no I/O.
# The src/compiler dir covers builder/SymbolSlots.hpp, the producer side of
# the placeholder-slot contract (header-only; no nlang_compiler symbols).
add_executable(test_nculinker
    test_vm/test_nculinker.cpp
)
target_include_directories(test_nculinker
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/src/compiler
        ${PROJECT_SOURCE_DIR}/include
        #ModuleRegistry.h includes public compiler headers by bare name
        #(same export nlang_compiler itself uses).
        ${PROJECT_SOURCE_DIR}/include/nlang/compiler
)
target_link_libraries(test_nculinker
    PRIVATE
        nlang_vm
        nlang_runtime
)
add_test(NAME nculinker_tests COMMAND test_nculinker)

# nloader unit tests: synthetic .ncu images and .npkg archives drive the
# runtime closure loader — entry selection, eager discovery from the
# symbol-import tables, member location precedence and the one-shot
# diagnostics contract. No codegen, no linking (nlink has its own suite).
add_executable(test_nculoader
    test_vm/test_nculoader.cpp
)
target_include_directories(test_nculoader
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_nculoader
    PRIVATE
        nlang_vm
        nlang_runtime
)
add_test(NAME nculoader_tests COMMAND test_nculoader)

# NativeHost ABI unit tests: mock host verifies the C ABI function table and
# the header-only C++ wrappers. No VM/shared library needed.
add_executable(test_native_abi
    test_vm/test_native_abi.cpp
)
target_include_directories(test_native_abi
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME native_abi_tests COMMAND test_native_abi)

# NativeLibraryLoader fixture shared modules (real DLLs loaded by the test).
add_library(nlang_sample SHARED
    fixtures/native/sample/sample_native.cpp
)
target_include_directories(nlang_sample PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_library(nlang_badabi SHARED
    fixtures/native/badabi/badabi_native.cpp
)
target_include_directories(nlang_badabi PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_library(nlang_noentry SHARED
    fixtures/native/noentry/noentry_native.cpp
)

# NativeLibraryLoader unit tests: real discovery/loading of the fixtures.
add_executable(test_native_loader
    test_vm/test_native_loader.cpp
)
target_include_directories(test_native_loader
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_native_loader
    PRIVATE nlang_vm
)
target_compile_definitions(test_native_loader
    PRIVATE
        FIXTURE_DIR="$<TARGET_FILE_DIR:nlang_sample>"
)
add_dependencies(test_native_loader nlang_sample nlang_badabi nlang_noentry)
add_test(NAME native_loader_tests COMMAND test_native_loader)

# LibrarySearchPath (header-only, pure STL) unit tests: layered ordering,
# environment splitting, normalization and de-duplication. No library link.
add_executable(test_library_search_path
    test_vm/test_library_search_path.cpp
)
target_include_directories(test_library_search_path
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME library_search_path_tests COMMAND test_library_search_path)

# NativeHost callback integration tests: real compile+run exercising every
# host callback through in-process registered natives.
add_executable(test_native_host
    test_vm/test_native_host.cpp
)
target_include_directories(test_native_host
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_native_host
    PRIVATE
        nlang_vm nlang_compiler nlang_runtime nlang_langservice
)
target_compile_definitions(test_native_host
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)
add_test(NAME native_host_tests COMMAND test_native_host)

# Third-party native library fixture (a real DLL shipped inside a package).
add_library(nlang_mylib SHARED
    fixtures/native/mylib/mylib_native.cpp
)
target_include_directories(nlang_mylib PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Mixed-library fixture: a second namespace/DLL so the in-process test can
# load two native packages in one process (Windows maps images by base name).
add_library(nlang_mixlib SHARED
    fixtures/native/mixlib/mixlib_native.cpp
)
target_include_directories(nlang_mixlib PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Third-party library integration tests: a package dir holding mylib.n and
# the nlang_mylib.dll fixture, imported and run through the same path as the
# standard library (real compile + real run).
add_executable(test_thirdparty
    test_vm/test_thirdparty.cpp
)
target_include_directories(test_thirdparty
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_thirdparty
    PRIVATE
        nlang_vm nlang_compiler nlang_runtime nlang_langservice
)
target_compile_definitions(test_thirdparty
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
        FIXTURE_DIR="$<TARGET_FILE_DIR:nlang_mylib>"
        MYLIB_DLL_NAME="$<TARGET_FILE_NAME:nlang_mylib>"
        MIXLIB_DLL_NAME="$<TARGET_FILE_NAME:nlang_mixlib>"
        MIX_SOURCE_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/native/mixlib"
)
add_dependencies(test_thirdparty nlang_mylib nlang_mixlib)
add_test(NAME thirdparty_tests COMMAND test_thirdparty)
add_executable(test_library_source
    test_vm/test_library_source.cpp
)
# .npkg container unit tests (phase 6 Task 2): writer/reader round trip,
# determinism, duplicate/checksum/magic/version rejections.
add_executable(test_ncu_package
    test_vm/test_ncu_package.cpp
)
target_link_libraries(test_ncu_package
    PRIVATE
        nlang_vm nlang_runtime
)
add_test(NAME ncu_package_tests COMMAND test_ncu_package)
target_include_directories(test_library_source
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_library_source
    PRIVATE
        nlang_vm nlang_compiler nlang_runtime nlang_langservice
)
target_compile_definitions(test_library_source
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)
add_test(NAME library_source_tests COMMAND test_library_source)

# Per-unit codegen + nlink integration tests (phase 6 Step 3/4): multi-file
# programs compiled per translation unit, linked from the unit images, and
# really executed — every cross-unit reference goes through a placeholder
# slot resolved by NcuLinker.
add_executable(test_perunit
    test_vm/test_perunit.cpp
)
target_include_directories(test_perunit
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_perunit
    PRIVATE
        nlang_vm nlang_compiler nlang_runtime nlang_langservice
)
target_compile_definitions(test_perunit
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)
add_test(NAME perunit_tests COMMAND test_perunit)

# ncc end-to-end test for a third-party native package: assemble a package
# directory holding the declaration (mylib.n), an example program
# (use_mylib.n) and the nlang_mylib.dll fixture, then compile and run through
# ncc with the package on the import path. The in-process thirdparty_tests
# above drives ModuleBuilder/VmExecutor directly; this one covers the CLI's
# native search wiring in main.cpp.
set(mylib_pkg_dir ${CMAKE_BINARY_DIR}/e2e_out/mylib_pkg)
#Phase 6: the library ships as a compiled unit beside its source — consumer
#builds keep inlining mylib.n for signatures, but its code joins the
#consumer's run-time closure from mylib.ncu (nothing is baked into the
#consumer artifact anymore). One build step here feeds every ncc/nvm/ndb
#entry below that consumes the package dir.
add_custom_target(nlang_mylib_pkg ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${mylib_pkg_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_SOURCE_DIR}/tests/fixtures/native/mylib/mylib.n
        ${PROJECT_SOURCE_DIR}/tests/fixtures/native/mylib/use_mylib.n
        ${mylib_pkg_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:nlang_mylib>
        ${mylib_pkg_dir}
    COMMAND $<TARGET_FILE:ncc> build ${mylib_pkg_dir}/mylib.n
        -o ${mylib_pkg_dir}/mylib.ncu
    DEPENDS ncc nlang_mylib
    VERBATIM)
#Parallel-build ordering: the stdlib bootstrap populates the build-tree
#stdlib dir this build resolves first (FindStdLibDir). Harmless today
#(mylib.n imports nothing), but a future fixture library importing the
#stdlib would race the bootstrap without this edge.
add_dependencies(nlang_mylib_pkg nlang_stdlib_package)

add_test(NAME ncc_thirdparty_run
    COMMAND ncc
        ${mylib_pkg_dir}/use_mylib.n
        -I ${mylib_pkg_dir}
        -o ${CMAKE_BINARY_DIR}/e2e_out/use_mylib.ncu)
set_tests_properties(ncc_thirdparty_run PROPERTIES
    PASS_REGULAR_EXPRESSION "hello, world"
    TIMEOUT 180)

# --- Default / environment search-path coverage (no -I) ---
# Base layer: the example, its mylib.n and the DLL all sit in the package
# (the source's own dir), so no -I is needed.
add_test(NAME ncc_thirdparty_default_run
    COMMAND ncc
        ${mylib_pkg_dir}/use_mylib.n
        -o ${CMAKE_BINARY_DIR}/e2e_out/default_run.ncu)
set_tests_properties(ncc_thirdparty_default_run PROPERTIES
    PASS_REGULAR_EXPRESSION "hello, world"
    TIMEOUT 180)

# Environment layer: a source copy with NO package beside it; the package is
# reached only through NLANG_PATH (single dir — ';' is CMake's list separator;
# multi-path splitting is covered by the unit tests).
set(env_src_dir ${CMAKE_BINARY_DIR}/e2e_out/env_src)
add_custom_target(nlang_mylib_env_src ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${env_src_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_SOURCE_DIR}/tests/fixtures/native/mylib/use_mylib.n
        ${env_src_dir}
    VERBATIM)
add_test(NAME ncc_thirdparty_env_run
    COMMAND ncc
        ${env_src_dir}/use_mylib.n
        -o ${CMAKE_BINARY_DIR}/e2e_out/env_run.ncu)
set_tests_properties(ncc_thirdparty_env_run PROPERTIES
    PASS_REGULAR_EXPRESSION "hello, world"
    ENVIRONMENT "NLANG_PATH=${mylib_pkg_dir}"
    DEPENDS nlang_mylib_env_src
    TIMEOUT 180)

# --- --verbose search-path observability (compile and run forms) ---
# The (-I) attribution line proves the resolved search listing was
# printed; the run itself still completes normally (the no-flag
# companion ncc_thirdparty_run pins the run outcome, and the e2e
# verbose_search_path entry pins print-then-proceed for the family).
add_test(NAME ncc_verbose_search_path
    COMMAND ncc
        ${mylib_pkg_dir}/use_mylib.n
        -I ${mylib_pkg_dir}
        --verbose
        -o ${CMAKE_BINARY_DIR}/e2e_out/verbose_run.ncu)
set_tests_properties(ncc_verbose_search_path PROPERTIES
    PASS_REGULAR_EXPRESSION "\\(-I\\)"
    DEPENDS nlang_mylib_pkg
    TIMEOUT 180)

# The `ncc run` form composes its own search path (module dir base).
add_test(NAME ncc_run_verbose_search_path
    COMMAND ncc
        run
        ${CMAKE_BINARY_DIR}/e2e_out/use_mylib.ncu
        -I ${mylib_pkg_dir}
        --verbose)
set_tests_properties(ncc_run_verbose_search_path PROPERTIES
    PASS_REGULAR_EXPRESSION "\\(-I\\)"
    DEPENDS ncc_thirdparty_run
    TIMEOUT 180)

# -v is the short form of --verbose; each tool parses its own command
# line, so each parse site gets one pin: the e2e verbose_short_flag and
# dbg_verbose_short entries cover nvm and ndb's shared CLI/machine
# parse, and these two cover ncc's separate main and `run` loops (run
# outcomes are pinned by the no-flag companions).
add_test(NAME ncc_run_verbose_short
    COMMAND ncc
        run
        ${CMAKE_BINARY_DIR}/e2e_out/use_mylib.ncu
        -I ${mylib_pkg_dir}
        -v)
set_tests_properties(ncc_run_verbose_short PROPERTIES
    PASS_REGULAR_EXPRESSION "\\(-I\\)"
    DEPENDS ncc_thirdparty_run
    TIMEOUT 180)

# The compile form parses -v in ncc's main loop (the e2e verbose_short_flag
# entry pins nvm's parse and dbg_verbose_short pins ndb's shared one).
add_test(NAME ncc_verbose_short
    COMMAND ncc
        ${mylib_pkg_dir}/use_mylib.n
        -I ${mylib_pkg_dir}
        -v
        -o ${CMAKE_BINARY_DIR}/e2e_out/verbose_short.ncu)
set_tests_properties(ncc_verbose_short PROPERTIES
    PASS_REGULAR_EXPRESSION "\\(-I\\)"
    DEPENDS nlang_mylib_pkg
    TIMEOUT 180)

# nvm native search: -I (DLL off-module) and the default module-dir search.
# A Python harness builds the module first (ctest regexes cannot sequence
# steps or copy the DLL beside the module).
if(Python3_FOUND)
    add_test(NAME nvm_native_paths
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_nvm_native.py
            $<TARGET_FILE:ncc> $<TARGET_FILE:nvm>
            ${mylib_pkg_dir} ${CMAKE_BINARY_DIR}/e2e_out)
    set_tests_properties(nvm_native_paths PROPERTIES TIMEOUT 180)

    # ndb --machine: the third-party DLL reached via -I, driven by `run`.
    add_test(NAME ndb_native_machine
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_ndb_native.py
            $<TARGET_FILE:ncc> $<TARGET_FILE:ndb>
            ${mylib_pkg_dir} ${CMAKE_BINARY_DIR}/e2e_out)
    set_tests_properties(ndb_native_machine PROPERTIES TIMEOUT 180)
endif()

# Phase 11 stdlib unit tests: in-process compile+run of real .n sources
# via ModuleBuilder (temp .ncu round-trip) + VmExecutor.
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
        nlang_langservice
)
target_compile_definitions(test_stdlib
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME stdlib_tests COMMAND test_stdlib)

# Generic instance array-type-arg unit tests (console suite).
# In-process ModuleBuilder compile of real .n sources, asserting the
# key split (List<int> vs List<int[]> mint distinct classes) and the
# interned SnArrayTypeToken carried in the type-arg slots.
add_executable(test_array_flags
    test_compiler/test_array_flags.cpp
)

target_include_directories(test_array_flags
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_array_flags
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)

add_test(NAME array_flags_tests COMMAND test_array_flags)

# 0.7.3 B: interned array type token unit tests (console suite). Drives
# BuildEnvironment::InternArrayTypeToken hash-consing identity and the
# GetCastInfo array matrix directly — the token has no tree writers yet.
add_executable(test_array_token
    test_compiler/test_array_token.cpp
)

target_include_directories(test_array_token
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_array_token
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)

add_test(NAME array_token_tests COMMAND test_array_token)

# BuildEnvironment library-index unit tests: a stdlib dir loads the
# declaration index; an empty dir leaves it empty.
add_executable(test_library_index
    test_compiler/test_library_index.cpp
)
target_include_directories(test_library_index
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_library_index
    PRIVATE
        nlang_compiler
        nlang_runtime
)
target_compile_definitions(test_library_index
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)
add_test(NAME library_index_tests COMMAND test_library_index)

# 0.7.5 basic types: scalar primitive registry unit tests (console
# suite). Pins the registry as the single source of truth — row order,
# slot widths, RTK allocation (0/1 + 10..19), kind/rtk round-trips, Rn
# singleton wiring, derived predicates and the post-expansion NK layout.
add_executable(test_primitivetypes
    test_compiler/test_primitivetypes.cpp
)

target_include_directories(test_primitivetypes
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_primitivetypes
    PRIVATE
        nlang_runtime
        nlang_compiler
        nlang_vm
)

add_test(NAME primitivetypes_tests COMMAND test_primitivetypes)

# 0.7.5 kind-immediate opcode family unit tests (console suite). Three
# layers: dispatch-table drives (kAddTable etc. on byte buffers — ops x
# all 12 registry kinds), the Rn formatters OP_Prim_to_str calls, and
# real compiled .n sources for handler guards (div/mod-zero messages,
# scalar+string concat end-to-end).
add_executable(test_primops
    test_vm/test_primops.cpp
)

target_include_directories(test_primops
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_primops
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)
target_compile_definitions(test_primops
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME primops_tests COMMAND test_primops)

# ndb debugger unit tests: .ncu v1.9 sourceFile, B.1 imported locals,
# debug hooks, read-only view, DebugSessionController session semantics,
# ndb CLI adapter formats, machine-mode line protocol. DebugSession.cpp,
# MachineFrontEnd.cpp and SourceCache.cpp are compiled into the test
# (the tool's main.cpp is not) so the sessions can be driven with string
# streams.
add_executable(test_debugger
    test_vm/test_debugger.cpp
    test_vm/test_debugger_common.cpp
    test_vm/test_debugger_format.cpp
    test_vm/test_debugger_gc.cpp
    test_vm/test_debugger_hooks.cpp
    test_vm/test_debugger_linepc.cpp
    test_vm/test_debugger_session.cpp
    test_vm/test_debugger_controller.cpp
    test_vm/test_debugger_machine.cpp
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
target_compile_definitions(test_debugger
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME debugger_tests COMMAND test_debugger)

# String object store + GC unit tests: in-process ModuleBuilder compile
# of real .n sources + VmExecutor runs with white-box GC observers
# (SetGcStressThresholds / LiveStringObjectCount).
add_executable(test_strings
    test_vm/test_strings.cpp
)
target_include_directories(test_strings
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_strings
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)
add_test(NAME string_gc_tests COMMAND test_strings)
set_tests_properties(string_gc_tests PROPERTIES TIMEOUT 180)

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
    #ProjectFile.cpp validates bytes via Utf8ContentCheck (public
    #header nlang/compiler/Utf8.h); ncc itself reaches both by linking
    #nlang_compiler, which this standalone target stays out of.
    ${PROJECT_SOURCE_DIR}/src/compiler/Utf8.cpp
)

target_include_directories(test_projectfile
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/tools/ncc
        ${PROJECT_SOURCE_DIR}/include
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
    target_compile_definitions(test_import_parse
        PRIVATE
            STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
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
    target_compile_definitions(test_module_import
        PRIVATE
            STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
    )
    if(WIN32)
        add_custom_command(TARGET test_module_import POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_module_import>)
    endif()
    nlang_add_qtest(module_import_tests test_module_import)

    # Array-valued type property unit tests (array redesign B Task 1):
    # IsArrayValued() is stamped at resolver binding tails from the
    # declared/bound type, so each expression shape (identifier, member
    # field, new-array, array-returning call, container element read,
    # container subscript) carries the property the old shape-inference
    # predicate computed post hoc. Driven in-process through
    # ModuleBuilder; the src include dirs cover later tasks reading the
    # built .ncu back through ModuleLoader.
    add_executable(test_array_property
        test_compiler/test_array_property.cpp
    )
    target_include_directories(test_array_property
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src/compiler
            ${PROJECT_SOURCE_DIR}/src/vm
    )
    target_link_libraries(test_array_property
        PRIVATE
            nlang_compiler
            nlang_runtime
            nlang_vm
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_array_property POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_array_property>)
    endif()
    nlang_add_qtest(array_property_tests test_array_property)

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

    # SettingsStore unit tests: pure state + resolvers, no widgets.
    add_executable(test_settingsstore
        test_nide/test_settingsstore.cpp
    )
    target_link_libraries(test_settingsstore
        PRIVATE
            nide_settings
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_settingsstore POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_settingsstore>)
    endif()
    nlang_add_qtest(settingsstore_tests test_settingsstore)

    # SearchPathArgs unit tests: pure functions turning project + global
    # library paths into -I arguments, no widgets.
    add_executable(test_searchpathargs
        test_nide/test_searchpathargs.cpp
    )
    target_link_libraries(test_searchpathargs
        PRIVATE
            nide_projectmodel
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_searchpathargs POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_searchpathargs>)
    endif()
    nlang_add_qtest(searchpathargs_tests test_searchpathargs)

    # Debug-protocol codec unit tests: the Qt-side mirror of the engine
    # codec (MachineFrontEnd.h documents the wire format; nide_debug
    # deliberately reimplements it, and the identical vectors in
    # tests/test_vm/test_debugger.cpp keep the two sides from drifting).
    add_executable(test_debug_protocol
        test_nide/test_debug_protocol.cpp
    )
    target_link_libraries(test_debug_protocol
        PRIVATE
            nide_debug
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_debug_protocol POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_debug_protocol>)
    endif()
    nlang_add_qtest(debug_protocol_tests test_debug_protocol)

    # DebugClient session tests: a real `ndb --machine` child plus a
    # real .ncu built by real ncc (no mocks; the real-tools pattern of
    # test_mainwindow). The tool paths arrive as compile definitions --
    # generator expressions, so forward slashes QProcess accepts as-is.
    add_executable(test_debug_client
        test_nide/test_debug_client.cpp
    )
    target_compile_definitions(test_debug_client
        PRIVATE
            NDB_EXE="$<TARGET_FILE:ndb>"
            NCC_EXE="$<TARGET_FILE:ncc>"
    )
    target_link_libraries(test_debug_client
        PRIVATE
            nide_debug
            Qt5::Test
    )
    add_dependencies(test_debug_client ndb ncc)
    if(WIN32)
        add_custom_command(TARGET test_debug_client POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_debug_client>)
    endif()
    nlang_add_qtest(debug_client_tests test_debug_client)

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
    # ncc/nvm/ndb for build, run and debug, so all three exes are copied
    # next to the test binary (MainWindow resolves them in the
    # application directory via toolPath -- no compile definitions).
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
    add_dependencies(test_mainwindow ncc nvm ndb)
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
                $<TARGET_FILE:ncc> $<TARGET_FILE:nvm> $<TARGET_FILE:ndb>
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
        TIMEOUT 180)

    # Library search-path end-to-end tests: a global library dir is indexed
    # and offered in completion and built via -I; a project <ImportPaths>
    # feeds the same path. Real ncc/nvm/ndb (no mocks).
    add_executable(test_searchpath_integration
        test_nide/test_searchpath_integration.cpp
    )
    target_link_libraries(test_searchpath_integration
        PRIVATE
            nide_mainwindow
            Qt5::Test
    )
    add_dependencies(test_searchpath_integration ncc nvm ndb)
    if(WIN32)
        add_custom_command(TARGET test_searchpath_integration POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
                $<TARGET_FILE:Qt5::Widgets> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_searchpath_integration>)
        add_custom_command(TARGET test_searchpath_integration POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:ncc> $<TARGET_FILE:nvm> $<TARGET_FILE:ndb>
                $<TARGET_FILE_DIR:test_searchpath_integration>)
    endif()
    nlang_add_qtest(searchpath_integration_tests test_searchpath_integration)
    set_tests_properties(searchpath_integration_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_PLATFORM_PLUGINS_DIR}")
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
    # nvm flag-position guard: --gc-stress=N must parse from any
    # argument position, with the module exit code unchanged.
    add_test(NAME nvm_gc_stress_position
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_nvm_gc_stress.py
            $<TARGET_FILE:nvm> $<TARGET_FILE:ncc>)
    set_tests_properties(nvm_gc_stress_position PROPERTIES TIMEOUT 60)
    # .ncu byte-reproducibility guard: the same compiler and source must
    # yield identical module bytes on every run (regression net for the
    # uninitialized class-field access serialization bug).
    add_test(NAME ncu_determinism
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_ncu_determinism.py
            $<TARGET_FILE:ncc>)
    set_tests_properties(ncu_determinism PROPERTIES TIMEOUT 180)
    # Phase 4c guard: no hardcoded standard library may return to the VM
    # (signature table, math/io/fs intrinsic families, their id blocks).
    add_test(NAME no_builtin_stdlib
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_no_builtin_stdlib.py)
    set_tests_properties(no_builtin_stdlib PROPERTIES TIMEOUT 60)
    # langservice: index real stdlib/*.n, resolve / complete / locate.
    add_executable(test_langservice
        test_langservice/test_symbol_index.cpp
    )
    target_link_libraries(test_langservice
        PRIVATE
            nlang_langservice
    )
    target_compile_definitions(test_langservice
        PRIVATE
            STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
    )
    add_test(NAME langservice_tests COMMAND test_langservice)
    set_tests_properties(langservice_tests PROPERTIES TIMEOUT 60)
    # UTF-8 path guard: the CLI executables' UTF-8 active code page
    # (manifest) must survive argv ingestion, file I/O, and echo — a
    # mixed CJK+emoji directory covers both the mojibake and the
    # '?'-destruction layer.
    add_test(NAME cli_utf8_paths
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_cli_utf8_paths.py
            $<TARGET_FILE:ncc> $<TARGET_FILE:nvm>
            $<TARGET_FILE:ndisasm> $<TARGET_FILE:ndb>
            ${CMAKE_BINARY_DIR})
    set_tests_properties(cli_utf8_paths PROPERTIES TIMEOUT 120)
    # .nproj UTF-8 contract guard: legacy-encoded project files are
    # rejected with a named UTF-8 error (UTF-8 BOM accepted and
    # skipped; UTF-16 gets a dedicated hint).
    add_test(NAME nproj_utf8
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_nproj_utf8.py
            $<TARGET_FILE:ncc> ${CMAKE_BINARY_DIR})
    set_tests_properties(nproj_utf8 PROPERTIES TIMEOUT 120)
    # Source-size guard: hand-written sources stay <=500 lines per file
    # and <=50 lines per function; registered exceptions live in
    # tools/source_size_guard/allowlist.json, each with a reason.
    add_test(NAME source_size_guard
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/source_size_guard/check_source_size.py)
    set_tests_properties(source_size_guard PROPERTIES TIMEOUT 60)
endif()

# End-to-end test: compile-only (exits 0)
add_test(NAME e2e_compile
    COMMAND ncc build ${PROJECT_SOURCE_DIR}/examples/hello.n)

# Project-mode compile: multi-source .nproj via -p (output redirected into
# the build tree so the source tree stays clean). Project mode packs the
# .npkg; no .ncu is written (phase 6 artifact flip).
add_test(NAME project_compile
    COMMAND ncc build -p ${PROJECT_SOURCE_DIR}/examples/hello_project/hello_project.nproj
    -o ${CMAKE_CURRENT_BINARY_DIR}/hello_project.npkg)

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
# the redirected output (the .npkg through the closure loader + link).
# main() maps the cross-file result to 0/1, so a clean pass means
# utils.n's addBoth(40, 2) really returned 42.
add_test(NAME project_run
    COMMAND ncc -p ${PROJECT_SOURCE_DIR}/examples/hello_project/hello_project.nproj
    -o ${CMAKE_CURRENT_BINARY_DIR}/hello_project_run.npkg)

# Library demo (per-unit artifacts): the library unit compiles on its
# own first (build form -- a library has no entry point); the consumer
# then resolves signatures from the source directory at compile time
# and loads mylib.ncu from -I at run time. All artifacts stay in the
# build tree; ncc's compile+run form doubles as the run gate.
add_test(NAME libdemo_compile
    COMMAND ncc build ${PROJECT_SOURCE_DIR}/examples/libdemo/mylib.n
        -o ${CMAKE_CURRENT_BINARY_DIR}/mylib.ncu)
set_tests_properties(libdemo_compile PROPERTIES TIMEOUT 180)
add_test(NAME libdemo_run
    COMMAND ncc ${PROJECT_SOURCE_DIR}/examples/libdemo/main.n
        -I ${CMAKE_CURRENT_BINARY_DIR}
        -I ${PROJECT_SOURCE_DIR}/examples/libdemo
        -o ${CMAKE_CURRENT_BINARY_DIR}/main_run.ncu)
set_tests_properties(libdemo_run
    PROPERTIES DEPENDS libdemo_compile TIMEOUT 180)

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
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_import_visibility.npkg)
add_test(NAME proj_import_visibility_run
    COMMAND nvm ${CMAKE_BINARY_DIR}/e2e_out/proj_import_visibility.npkg)
set_tests_properties(proj_import_visibility_run PROPERTIES
    DEPENDS proj_import_visibility_compile
    PASS_REGULAR_EXPRESSION "3")

#D7: a same-directory function is reachable bare AND qualified; the
#fixture self-checks both forms agree (7) before printing.
add_test(NAME proj_same_dir_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_same_dir/proj_same_dir.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_same_dir.npkg)
add_test(NAME proj_same_dir_run
    COMMAND nvm ${CMAKE_BINARY_DIR}/e2e_out/proj_same_dir.npkg)
set_tests_properties(proj_same_dir_run PROPERTIES
    DEPENDS proj_same_dir_compile
    PASS_REGULAR_EXPRESSION "7")

#Phase 5 D13 gate: ndisasm -func filters by the TABLE KEY, which is now
#package-qualified — the project free function f in extra.n is keyed
#"extra.f". Before the key change this printed nothing (bare "f" no
#longer exists); the output assertion (not the exit code: -func misses
#exit 0) is what makes the spelling change loud. The trailing space
#separates the key from a hypothetical "extra.fx". Consumes the
#per-unit proj_same_dir.npkg produced above (ndisasm iterates package
#members in the raw view; no new fixture, no new artifact).
add_test(NAME ndisasm_qualified_func_name
    COMMAND $<TARGET_FILE:ndisasm> -func extra.f
        ${CMAKE_BINARY_DIR}/e2e_out/proj_same_dir.npkg)
set_tests_properties(ndisasm_qualified_func_name PROPERTIES
    DEPENDS proj_same_dir_compile
    PASS_REGULAR_EXPRESSION "function extra\\.f ")

#Duplicate and overlapping imports (exact repeat + exact/wildcard
#overlap) are idempotent: the project compiles clean.
add_test(NAME proj_dup_import_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_dup_import/proj_dup_import.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_dup_import.npkg)

#D11 executed: the wildcard's union reaches the EXACT root module
#(utils.f, main lives in app/ so D7 cannot mask it) and the recursive
#prefix (utils.helper.help); the fixture self-checks both values before
#printing, so a wrong value exits 9 with empty output.
add_test(NAME proj_wildcard_exact_union_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_wildcard_exact_union/proj_wildcard_exact_union.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_wildcard_exact_union.npkg)
add_test(NAME proj_wildcard_exact_union_run
    COMMAND nvm ${CMAKE_BINARY_DIR}/e2e_out/proj_wildcard_exact_union.npkg)
set_tests_properties(proj_wildcard_exact_union_run PROPERTIES
    DEPENDS proj_wildcard_exact_union_compile
    PASS_REGULAR_EXPRESSION "6")

#Phase 6: project-mode builds pack the .npkg (one member per unit, the
#entry record naming the entry unit and function) even when a library
#unit (helper.n) leads the source list, and the package runs under nvm
#as a first-class artifact through the closure loader + load-time link.
#Tool-chain coverage only (the in-process pin is test_perunit shape 12):
#ncc and nvm as real subprocesses, exactly how users build and run
#program packages.
add_test(NAME proj_pkg_run_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_pkg_run/proj_pkg_run.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_pkg_run.npkg)
add_test(NAME proj_pkg_run_run
    COMMAND nvm ${CMAKE_BINARY_DIR}/e2e_out/proj_pkg_run.npkg)
set_tests_properties(proj_pkg_run_run PROPERTIES
    DEPENDS proj_pkg_run_compile
    PASS_REGULAR_EXPRESSION "42")

#Spec section 7 row 1: a known module path this TU did not import.
add_test(NAME proj_not_imported_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_not_imported/proj_not_imported.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_not_imported.npkg)
set_tests_properties(proj_not_imported_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "is not imported")

#Spec section 7: the bare pool never spans directories.
add_test(NAME proj_bare_cross_dir_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_bare_cross_dir/proj_bare_cross_dir.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_bare_cross_dir.npkg)
set_tests_properties(proj_bare_cross_dir_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "is not visible here")

#Spec section 5.2: project path segments may not use builtin namespace
#names (io here).
#Task 6: two search roots offering the same package is a build error
#naming both source paths (the reserved-name table's replacement).
add_test(NAME duplicate_package_path_compile
    COMMAND ncc build
        -I ${PROJECT_SOURCE_DIR}/tests/e2e/duplicate_package_path/roots/r1
        -I ${PROJECT_SOURCE_DIR}/tests/e2e/duplicate_package_path/roots/r2
        -p ${PROJECT_SOURCE_DIR}/tests/e2e/duplicate_package_path/duplicate_package_path.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/duplicate_package_path.ncu)
set_tests_properties(duplicate_package_path_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "Duplicate package 'graphics'")

#Getting-started FAQ pin: without -o the .ncu lands in the process CWD,
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
    proj_same_dir_compile proj_same_dir_run ndisasm_qualified_func_name
    proj_dup_import_compile
    proj_wildcard_exact_union_compile proj_wildcard_exact_union_run
    proj_pkg_run_compile proj_pkg_run_run
    proj_not_imported_compile proj_bare_cross_dir_compile
    duplicate_package_path_compile ncc_no_o_output_cwd
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

# Make the stdlib native modules discoverable to in-process VM tests. The
# loader's default search path is the host executable's directory; a test exe
# lives in tests/<config>, so copy nlang_math/io/fs there as part of the
# default test build. Tools (ncc/nvm/ndb) launched as test subprocesses use
# the copies deployed next to them, so this only serves tests driving the VM
# in-process (test_stdlib, test_debugger, test_module_import, ...). Depending
# on the module targets guarantees the DLLs are built and current first.
add_custom_target(nlang_stdlib_native_for_tests ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/tests/$<CONFIG>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:nlang_math>
                $<TARGET_FILE:nlang_io>
                $<TARGET_FILE:nlang_fs>
                "${CMAKE_BINARY_DIR}/tests/$<CONFIG>"
    VERBATIM)
add_dependencies(nlang_stdlib_native_for_tests nlang_math nlang_io nlang_fs)

# STDLIB_DIR (NLANG_TEST_STDLIB_DIR, see the top of this file) is the
# build-tree stdlib copy: its sources and stdlib.npkg are produced by
# nlang_stdlib_package. Every test that compiles or loads against that
# dir depends on the package target, so a targeted build of just these
# tests still sees a populated dir (a missed entry here only bites
# targeted builds on a fresh tree; full builds always pack first).
foreach(t IN ITEMS
        test_native_host test_thirdparty test_library_source test_perunit
        test_stdlib test_library_index test_debugger test_import_parse
        test_module_import test_langservice test_primops)
    add_dependencies(${t} nlang_stdlib_package)
endforeach()

