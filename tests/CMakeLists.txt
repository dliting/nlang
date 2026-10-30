# Stdlib dir every STDLIB_DIR-defining test compiles and loads against:
# the BUILD-TREE copy (root CMakeLists "Standard library package") — the
# same dir the tools resolve via FindStdLibDir. It carries the library
# sources for compile-time signature resolution AND stdlib.npkg for the
# load-time closure (NcuLoader search dirs), so tests resolve stdlib
# exactly like production instead of half source-tree, half nowhere.
# Each such test calls nlang_stdlib_test() at its definition site,
# ordering it after the package producer so targeted builds work on a
# fresh build tree too.
set(NLANG_TEST_STDLIB_DIR ${CMAKE_BINARY_DIR}/stdlib)

# Tests that compile or load against NLANG_TEST_STDLIB_DIR call this
# helper right after their target definition: it orders the build after
# nlang_stdlib_package, the producer of the dir's sources and
# stdlib.npkg, so a targeted build of just the test still sees a
# populated dir on a fresh tree (full builds always pack first). Kept as
# a per-test call at the definition site -- the former centralized list
# at the end of this file kept drifting when new tests were added.
function(nlang_stdlib_test target)
    add_dependencies(${target} nlang_stdlib_package)
endfunction()

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
target_include_directories(nlang_noentry PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_library(nlang_noprobe SHARED
    fixtures/native/noprobe/noprobe_native.cpp
)
target_include_directories(nlang_noprobe PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_library(nlang_badreturn SHARED
    fixtures/native/badreturn/badreturn_native.cpp
)
target_include_directories(nlang_badreturn PRIVATE ${PROJECT_SOURCE_DIR}/include)

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
add_dependencies(test_native_loader nlang_sample nlang_badabi
    nlang_noentry nlang_noprobe nlang_badreturn nlang_io)
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
nlang_stdlib_test(test_native_host)
target_compile_definitions(test_native_host
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)
add_test(NAME native_host_tests COMMAND test_native_host)

# Input-side unit tests: InputLineSource (injected FILE*) and TokenView
# (scripted line source) — slicing logic and EOF semantics, no executor.
add_executable(test_io_input
    test_vm/test_io_input.cpp
)
target_include_directories(test_io_input
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_io_input
    PRIVATE nlang_vm nlang_runtime
)
add_test(NAME io_input_tests COMMAND test_io_input)
set_tests_properties(io_input_tests PROPERTIES TIMEOUT 180)

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
nlang_stdlib_test(test_thirdparty)
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
nlang_stdlib_test(test_library_source)
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
nlang_stdlib_test(test_perunit)
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
nlang_stdlib_test(test_stdlib)
target_compile_definitions(test_stdlib
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME stdlib_tests COMMAND test_stdlib)

# NativeHost ABI v7 array-access tests: real VmExecutor + bare-name
# in-process natives exercising getArrayView/newArray end to end
# (test_native_abi covers the mock-table face, this the real-VM face).
add_executable(test_native_array_abi
    test_vm/test_native_array_abi.cpp
)

target_include_directories(test_native_array_abi
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_native_array_abi
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
        nlang_langservice
)
nlang_stdlib_test(test_native_array_abi)
target_compile_definitions(test_native_array_abi
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME native_array_abi_tests COMMAND test_native_array_abi)

# Stage-2 task 1: rank-carrying array type tokens (comma suffix grammar,
# jagged gate preservation, generic-argument acceptance).
add_executable(test_nd_tokens
    test_vm/test_nd_tokens.cpp
)

target_include_directories(test_nd_tokens
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_nd_tokens
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
        nlang_langservice
)
nlang_stdlib_test(test_nd_tokens)
target_compile_definitions(test_nd_tokens
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME nd_tokens_tests COMMAND test_nd_tokens)

# Stage-2 task 2: comma index lists and ND creation resolution.
add_executable(test_nd_resolve
    test_vm/test_nd_resolve.cpp
)

target_include_directories(test_nd_resolve
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_nd_resolve
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
        nlang_langservice
)
nlang_stdlib_test(test_nd_resolve)
target_compile_definitions(test_nd_resolve
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME nd_resolve_tests COMMAND test_nd_resolve)

# Stride-table integrity: every opcode resolves to an extent in
# [1, 9] (the rewrite regression — inline-braced constexpr rows
# silently failed to match under MSVC).
add_executable(test_stride_table
    test_vm/test_stride_table.cpp
)

target_include_directories(test_stride_table
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_stride_table
    PRIVATE
        nlang_vm
        nlang_runtime
)

add_test(NAME stride_table_tests COMMAND test_stride_table)

# 0.9.11 D2 struct constructor unit tests (console suite): in-process
# compile+run pinning that the ctor runs on explicit construction calls
# only, never on bare declarations (test_stdlib's runSource shape).
add_executable(test_struct_ctor
    test_vm/test_struct_ctor.cpp
)

target_include_directories(test_struct_ctor
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_struct_ctor
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)
nlang_stdlib_test(test_struct_ctor)
target_compile_definitions(test_struct_ctor
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME struct_ctor_tests COMMAND test_struct_ctor)

# 0.9.11 D4 struct field initializer unit tests (console suite):
# in-process compile+run pinning that field defaults apply at every
# allocation (bare decl, array elements, nested fields), brace/ctor
# stores override them, deep copy never re-applies them, and minted
# string defaults survive GC pressure (test_struct_ctor's runSource
# shape + test_strings' SetGcStressThresholds hook).
add_executable(test_struct_field_init
    test_vm/test_struct_field_init.cpp
)

target_include_directories(test_struct_field_init
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_struct_field_init
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)
nlang_stdlib_test(test_struct_field_init)
target_compile_definitions(test_struct_field_init
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME struct_field_init_tests COMMAND test_struct_field_init)

# 0.9.12 T1 VmClassLayout unit tests (console suite): hand-built
# CompiledClass records (no codegen, no linking) pinning that the
# runtime instance-field layout view mirrors the records field-for-field
# while the producer still ships flattened arrays (T2 switches Build to
# the base-first chain join).
add_executable(test_class_layout
    test_vm/test_class_layout.cpp
)

target_include_directories(test_class_layout
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_class_layout
    PRIVATE
        nlang_vm
        nlang_runtime
)

add_test(NAME class_layout_tests COMMAND test_class_layout)

# Generic instance array-type-arg unit tests (console suite).
# In-process ModuleBuilder compile of real .n sources, asserting the
# key split (List<int> vs List<int[]> mint distinct classes) and the
# interned SnArrayTypeToken carried in the type-arg slots.
add_executable(test_array_flags
    test_compiler/test_array_flags.cpp
)

target_include_directories(test_array_flags
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/compiler
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

# v2.3 ExprDesc default-expression grammar drift guards (console suite).
# Real compile -> .ncu write -> load round-trip decoded by an INDEPENDENT
# hand-rolled reader (writer/reader co-drift cannot pass), the producer
# fold/RTK_Unfoldable boundary, SynthDefaultExpr malformed-stream rejects
# and the ExprDesc.h link-time remap (NcuLinker's corrupt-image gate).
# src/compiler/builder covers CompiledModuleDefaultSupport.hpp (internal
# header chain, shallow — see the test file header).
add_executable(test_default_exprdesc
    test_compiler/test_default_exprdesc.cpp
)

target_include_directories(test_default_exprdesc
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/src/compiler/builder
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_default_exprdesc
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)

add_test(NAME default_exprdesc_tests COMMAND test_default_exprdesc)

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
nlang_stdlib_test(test_library_index)
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
nlang_stdlib_test(test_primops)
target_compile_definitions(test_primops
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME primops_tests COMMAND test_primops)

# 并发 K1 单元测试（帧栈机/调度/挂起/根集）：真实 .n 源码编译 + 真实
# 执行，harness 形态同 test_primops Layer 3，随切片增长。
add_executable(test_threads
    test_vm/test_threads.cpp
)
target_include_directories(test_threads
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_threads
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)
nlang_stdlib_test(test_threads)
target_compile_definitions(test_threads
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME threads_tests COMMAND test_threads)

# K6 Task 1: GC stop-the-world rendezvous protocol white-box tests
# (real threads; no compiler/stdlib involvement).
add_executable(test_gcworldstop
    test_vm/test_gcworldstop.cpp
)
target_include_directories(test_gcworldstop
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_gcworldstop
    PRIVATE
        nlang_vm
)

add_test(NAME gcworldstop_tests COMMAND test_gcworldstop)

# K8 Task 1: domain lock set (mutual exclusion, nesting markers, order
# tripwire, overlap, telemetry) — real threads, no executor.
add_executable(test_domainlocks
    test_vm/test_domainlocks.cpp
)
target_include_directories(test_domainlocks
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_domainlocks
    PRIVATE
        nlang_vm
)

add_test(NAME domainlocks_tests COMMAND test_domainlocks)

# K8 Task 2: chunked block storage (stable element addresses, atomic size
# publication) — real threads for the read/growth overlap contract.
add_executable(test_chunkedarray
    test_vm/test_chunkedarray.cpp
)
target_include_directories(test_chunkedarray
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_chunkedarray
    PRIVATE
        nlang_vm
)

add_test(NAME chunkedarray_tests COMMAND test_chunkedarray)

# K8 Task 3: sharded free lists (own-shard lock-free pop, Growth-locked
# block refill, sweep redistribution) — real threads for uniqueness.
add_executable(test_shardalloc
    test_vm/test_shardalloc.cpp
)
target_include_directories(test_shardalloc
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_shardalloc
    PRIVATE
        nlang_vm
)

add_test(NAME shardalloc_tests COMMAND test_shardalloc)

# K3 task 5: stdlib concurrent package (pure-NLang WaitGroup/Semaphore)
# behavior tests — same harness shape as test_threads.
add_executable(test_stdlib_concurrency
    test_vm/test_stdlib_concurrency.cpp
)
target_include_directories(test_stdlib_concurrency
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_stdlib_concurrency
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)
nlang_stdlib_test(test_stdlib_concurrency)
target_compile_definitions(test_stdlib_concurrency
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME stdlib_concurrency_tests COMMAND test_stdlib_concurrency)

# builtin-decls phase 2: binding-table consistency guard for the declared
# generic prelude classes (List/Dict/Channel/Future) — stamped intrinsic
# records, the two-ctor Channel surface, the ctor-less Future, and the
# unbounded channel ctor contract.
add_executable(test_builtin_binding
    test_vm/test_builtin_binding.cpp
)
target_include_directories(test_builtin_binding
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_builtin_binding
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)
nlang_stdlib_test(test_builtin_binding)
target_compile_definitions(test_builtin_binding
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME builtin_binding_tests COMMAND test_builtin_binding)

# ndb debugger unit tests: .ncu v1.9 sourceFile, B.1 imported locals,
# debug hooks, read-only view, DebugSessionController session semantics,
# ndb CLI adapter formats, machine-mode line protocol. DebugSession.cpp,
# DebugSessionInspect.cpp, MachineFrontEnd.cpp and SourceCache.cpp are
# compiled into the test
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
    test_vm/test_debugger_multicontext.cpp
    test_vm/test_debugger_k7.cpp
    test_vm/test_backtrace.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ndb/DebugSession.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ndb/DebugSessionInspect.cpp
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
nlang_stdlib_test(test_debugger)
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

# Script-mode wrap unit tests (v0.9.1): entry synthesis driven in
# process through the real build -> load -> link pipeline; entry
# selection/signature asserted on the linked function table, the two
# rejection paths on their diagnostic text.
add_executable(test_script_wrap
    test_compiler/test_script_wrap.cpp
)

target_include_directories(test_script_wrap
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_script_wrap
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)

target_compile_definitions(test_script_wrap
    PRIVATE
        STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
)

add_test(NAME script_wrap_tests COMMAND test_script_wrap)
set_tests_properties(script_wrap_tests PROPERTIES TIMEOUT 180)

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
    nlang_stdlib_test(test_import_parse)
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

    # Generic class declaration grammar tests (promise-select Task 1):
    # in-process through ModuleBuilder, same shape as test_import_parse.
    add_executable(test_generic_decl
        test_compiler/test_generic_decl.cpp
    )
    #src/compiler: builder/ModuleRegistry.h (the second-build-seam test
    #reads Registry().SyntheticBuiltins(); same internal-header reach as
    #test_array_flags).
    target_include_directories(test_generic_decl
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src/compiler
    )
    target_link_libraries(test_generic_decl
        PRIVATE
            nlang_compiler
            nlang_runtime
            nlang_vm
            Qt5::Test
    )
    nlang_stdlib_test(test_generic_decl)
    target_compile_definitions(test_generic_decl
        PRIVATE
            STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
    )
    if(WIN32)
        add_custom_command(TARGET test_generic_decl POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_generic_decl>)
    endif()
    nlang_add_qtest(generic_decl_tests test_generic_decl)

    # Generic instantiation descriptor unit tests (v0.12.3 Task 3): pure
    # C++ coverage of GenericInstDescriptor — canonical spelling, intern
    # identity, nested/array arguments. No compilation, so no stdlib
    # deployment needed.
    add_executable(test_generic_descriptor
        test_vm/test_generic_descriptor.cpp
    )
    target_include_directories(test_generic_descriptor
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src/vm
    )
    target_link_libraries(test_generic_descriptor
        PRIVATE
            nlang_vm
            nlang_runtime
            Qt5::Test
    )
    nlang_add_qtest(generic_descriptor_tests test_generic_descriptor)

    # Type-parameter slot-kind unit tests (v0.12.3 Task 4): the RTK_TypeParam
    # label, the placeholder node predicate and the compile-time kind
    # emission + the .ncu byte round-trip. Links the compiler for the
    # registry/AST mint and the vm for VmBackend::RuntimeTypeKind (same
    # three-lib shape as test_generic_decl).
    add_executable(test_generic_typeparam_gc
        test_vm/test_generic_typeparam_gc.cpp
    )
    target_include_directories(test_generic_typeparam_gc
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src/vm
    )
    target_link_libraries(test_generic_typeparam_gc
        PRIVATE
            nlang_compiler
            nlang_runtime
            nlang_vm
            Qt5::Test
    )
    nlang_add_qtest(generic_typeparam_gc_tests test_generic_typeparam_gc)

    # Prelude String/Array face declarations (v0.12.2 Task 1): in-process
    # ModuleBuilder coverage of the two declaration classes — member
    # counts, Object-chain exclusion (spec D10), and intrinsic-channel
    # stamping of the declared records (same internal-header reach as
    # test_generic_decl, plus builder/ExprResolver.h for
    # FindPreludeClassDecl).
    add_executable(test_prelude_faces
        test_compiler/test_prelude_faces.cpp
    )
    target_include_directories(test_prelude_faces
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src/compiler
    )
    target_link_libraries(test_prelude_faces
        PRIVATE
            nlang_compiler
            nlang_runtime
            nlang_vm
            Qt5::Test
    )
    nlang_stdlib_test(test_prelude_faces)
    target_compile_definitions(test_prelude_faces
        PRIVATE
            STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}"
    )
    if(WIN32)
        add_custom_command(TARGET test_prelude_faces POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_prelude_faces>)
    endif()
    nlang_add_qtest(prelude_faces_tests test_prelude_faces)

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
    nlang_stdlib_test(test_module_import)
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

    # Statement-level diagnostic behavior unit tests (0.8.3 func batch):
    # pin that local-decl initializer errors are reported ONCE — the
    # decomposition inserts an AssignStmt mid-walk and the paragraph
    # walk revisits it, so every error exit of Access(SnAssignStmt) must
    # flag NF_Resolved. Each case asserts the diagnostic COUNT (the e2e
    # manifest only does substring-existence, which cannot catch a
    # double-report regression).
    add_executable(test_statement_diag
        test_compiler/test_statement_diag.cpp
    )
    #nlang_vm is required at link time (nlang_compiler references the VM
    #backend/module-loader symbols in the build path) — same shape as
    #test_array_property.
    target_link_libraries(test_statement_diag
        PRIVATE
            nlang_compiler
            nlang_runtime
            nlang_vm
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_statement_diag POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_statement_diag>)
    endif()
    nlang_add_qtest(statement_diag_tests test_statement_diag)

    #Override-signature / interface-contract diagnostics (P1): asserts
    #diagnostic COUNTS (the e2e manifest only does substring existence)
    #and the alias type-node identity fact the signature predicate sits on.
    add_executable(test_method_contracts
        test_compiler/test_method_contracts.cpp
    )
    target_link_libraries(test_method_contracts
        PRIVATE
            nlang_compiler
            nlang_runtime
            nlang_vm
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_method_contracts POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_method_contracts>)
    endif()
    nlang_add_qtest(method_contracts_tests test_method_contracts)

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

    # Run-error browser unit tests (v0.8.5): same harness shape as the
    # compile-log browser tests.
    add_executable(test_runerrorbrowser
        test_nide/test_runerrorbrowser.cpp
    )
    target_link_libraries(test_runerrorbrowser
        PRIVATE
            nide_compilelog
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_runerrorbrowser POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
                $<TARGET_FILE:Qt5::Widgets> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_runerrorbrowser>)
    endif()
    nlang_add_qtest(runerrorbrowser_tests test_runerrorbrowser)
    set_tests_properties(runerrorbrowser_tests PROPERTIES
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
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_PLATFORM_PLUGINS_DIR}"
        # House rule is a 3-minute ceiling, but this suite is the one
        # entry that drives dozens of real ncc/nvm/ndb child sessions;
        # on a loaded machine (e.g. right after a full rebuild, when
        # every child spawn is AV-rescanned) it needs well over 3
        # minutes. 5 minutes still catches a hang.
        TIMEOUT 300)

    # Deployment self-containment check (Phase 10 Step 9). Runs a
    # curated five-test probe subset from a scratch copy of the nide
    # build output with the plugin path cleared (empty env entry: no
    # extra search path) and a qt.conf pinning Qt to the copy -- one
    # test per resolution class (Qt stack, stdlib, ncc/nvm, PTY, ndb)
    # must resolve purely from the copy. See nide_deploy_check.cmake
    # for the division of concerns vs the full in-tree suite.
    add_test(NAME nide_deploy_check
        COMMAND ${CMAKE_COMMAND}
            -DDEPLOY_DIR=$<TARGET_FILE_DIR:nide>
            -DTEST_EXE=$<TARGET_FILE:test_mainwindow>
            -DQT_TEST_DLL=$<TARGET_FILE:Qt5::Test>
            -DNLANG_NATIVE_DLLS=$<TARGET_FILE:nlang_math>\;$<TARGET_FILE:nlang_io>\;$<TARGET_FILE:nlang_fs>\;$<TARGET_FILE:nlang_tensor>
            -DSTDLIB_DIR=${NLANG_TEST_STDLIB_DIR}
            #Scratch nested 9 levels under <build>/tests: the loader's
            #build-tree fallback walks up at most 8, so it cannot rescue
            #the check from <build>/native — resolution must come from
            #the copied install-form layout (the property under test).
            -DCHECK_DIR=${CMAKE_CURRENT_BINARY_DIR}/nide_deploy_check/c1/c2/c3/c4/c5/c6/c7/c8
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test_nide/nide_deploy_check.cmake)
    set_tests_properties(nide_deploy_check PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH="
        # 150 s inner bound (see nide_deploy_check.cmake) + the
        # scratch-copy script overhead.
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

    # Embedded terminal unit tests: emulator over vendored libvterm,
    # widget input/render model, pty backend with real child processes.
    add_executable(test_terminal
        test_nide/test_terminal.cpp
    )
    target_link_libraries(test_terminal
        PRIVATE
            nide_mainwindow
            Qt5::Test
    )
    if(WIN32)
        add_custom_command(TARGET test_terminal POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt5::Core> $<TARGET_FILE:Qt5::Gui>
                $<TARGET_FILE:Qt5::Widgets> $<TARGET_FILE:Qt5::Test>
                $<TARGET_FILE_DIR:test_terminal>)
    endif()
    nlang_add_qtest(terminal_tests test_terminal)
    #nlang_add_qtest already applies the house 180s TIMEOUT; only the
    #platform plugin path is specific to this test.
    set_tests_properties(terminal_tests PROPERTIES
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
    add_test(NAME version_nlang
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_version.py
            $<TARGET_FILE:nlang> nlang ${NLANG_VERSION})
    set_tests_properties(version_ncc version_nvm version_ndisasm version_ndb
        version_nlang
        PROPERTIES TIMEOUT 60)
    # nvm flag-position guard: --gc-stress=N must parse from any
    # argument position, with the module exit code unchanged.
    add_test(NAME nvm_gc_stress_position
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_nvm_gc_stress.py
            $<TARGET_FILE:nvm> $<TARGET_FILE:ncc>)
    set_tests_properties(nvm_gc_stress_position PROPERTIES TIMEOUT 60)
    # K2 slice 6: --carriers=N is parity-neutral for a spawn-free program
    # and refuses out-of-range values with an explicit diagnostic.
    add_test(NAME nvm_carriers_flag
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_nvm_carriers.py
            $<TARGET_FILE:nvm> $<TARGET_FILE:ncc>)
    set_tests_properties(nvm_carriers_flag PROPERTIES TIMEOUT 60)
    # nlang script runner: the three behavior faces of the tool (script
    # mode end to end incl. the D3 empty-script mapping, -c inline
    # statements incl. the non-ASCII wide-argv path, and the CLI
    # rejection surface). One checker, one ctest entry per face.
    foreach(nlang_case IN ITEMS run_script c_inline cli_rejects)
        add_test(NAME nlang_${nlang_case}
            COMMAND ${PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/check_nlang_tool.py
                ${nlang_case} $<TARGET_FILE:nlang>)
    endforeach()
    set_tests_properties(nlang_run_script nlang_c_inline nlang_cli_rejects
        PROPERTIES TIMEOUT 120)
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
    nlang_stdlib_test(test_langservice)
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

#0.9.0 Phase A: qualified constructor call without `new`
#(shapes.Point(2, 3)). The fixture self-checks the field sum (5) BEFORE
#printing, so the regex cannot pass on a wrong value (the match
#overrides the exit code).
add_test(NAME proj_qualified_ctor_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_qualified_ctor/proj_qualified_ctor.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_qualified_ctor.npkg)
add_test(NAME proj_qualified_ctor_run
    COMMAND nvm ${CMAKE_BINARY_DIR}/e2e_out/proj_qualified_ctor.npkg)
set_tests_properties(proj_qualified_ctor_run PROPERTIES
    DEPENDS proj_qualified_ctor_compile
    PASS_REGULAR_EXPRESSION "6")

#0.9.0 review follow-up: a diagnosed module-qualified failure (the
#generic-name hint, function-not-found) marks the member resolved; an
#enclosing member access must degrade to a clean compile failure, not
#an ncc crash. PASS pins the primary diagnostic (also printed before
#the crash), FAIL the crash banner — the pair separates the clean
#failure from the 0xC0000005 exit the null-field deref produced.
add_test(NAME proj_qual_generic_hint_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_qual_generic_hint/proj_qual_generic_hint.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_qual_generic_hint.npkg)
set_tests_properties(proj_qual_generic_hint_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "cannot be constructed by bare name"
    FAIL_REGULAR_EXPRESSION "internal crash")

add_test(NAME proj_qual_fail_chain_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_qual_fail_chain/proj_qual_fail_chain.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_qual_fail_chain.npkg)
set_tests_properties(proj_qual_fail_chain_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "does not exist"
    FAIL_REGULAR_EXPRESSION "internal crash")

#Void-support gap: member access on a void call result (the bare
#spelling is the e2e void_result_member entry; both route through the
#same guard in Access(SnMemberExpr&)).
add_test(NAME proj_void_result_member_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_void_result_member/proj_void_result_member.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_void_result_member.npkg)
set_tests_properties(proj_void_result_member_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "cannot access a member of the result of void"
    FAIL_REGULAR_EXPRESSION "internal crash")

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

#v0.9.1 script mode (project-form fixtures; same ctest-only shape as the
#matrix above — the e2e runner gates only single-file and order.txt
#layouts). Three behaviors: two entry-carrying script units in one
#project fail backend entry disambiguation (the files sit in different
#directories so the merged-namespace frontend stays out of the way); a
#script importing a library runs it through the whole chain (lib
#pre-built to e2e_out per the library-unit contract — signatures inline
#from the source dir at compile time, code joins the closure at load
#time; the fixture self-checks twice(21)==42 and maps to 0/9 like
#libdemo_run, since ctest has no exact-exit-code property); a library
#unit carrying top-level statements is rejected at the consumer's
#compile (D14).
add_test(NAME proj_two_scripts_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_two_scripts/proj_two_scripts.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_two_scripts.npkg)
set_tests_properties(proj_two_scripts_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "entry point is ambiguous")

add_test(NAME proj_imports_script_lib
    COMMAND ncc build
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_imports_script/lib.n
        #lib.ncu is the generic name the loader's module-name lookup
        #forces (bare `import lib` resolves <dir>/lib.ncu; a renamed
        #artifact breaks resolution). Sole producer in e2e_out today;
        #a future fixture importing a bare "lib" would share it.
        -o ${CMAKE_BINARY_DIR}/e2e_out/lib.ncu)

add_test(NAME proj_imports_script_compile
    COMMAND ncc build
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_imports_script/main.n
        -I ${PROJECT_SOURCE_DIR}/tests/e2e/proj_imports_script
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_imports_script.ncu)
set_tests_properties(proj_imports_script_compile PROPERTIES
    DEPENDS proj_imports_script_lib)

add_test(NAME proj_imports_script_run
    COMMAND nvm ${CMAKE_BINARY_DIR}/e2e_out/proj_imports_script.ncu)
set_tests_properties(proj_imports_script_run PROPERTIES
    DEPENDS proj_imports_script_compile)

add_test(NAME proj_imports_lib_stmts_compile
    COMMAND ncc build
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_imports_lib_stmts/main.n
        -I ${PROJECT_SOURCE_DIR}/tests/e2e/proj_imports_lib_stmts
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_imports_lib_stmts.ncu)
set_tests_properties(proj_imports_lib_stmts_compile PROPERTIES
    PASS_REGULAR_EXPRESSION "contains top-level statements")

#Project-mode entries are fixture compiles/runs; the FAQ CWD pin runs a
#real compile too. Keep each well under the 3-minute unit-test ceiling.
set_tests_properties(proj_import_visibility_compile proj_import_visibility_run
    proj_same_dir_compile proj_same_dir_run ndisasm_qualified_func_name
    proj_dup_import_compile
    proj_wildcard_exact_union_compile proj_wildcard_exact_union_run
    proj_pkg_run_compile proj_pkg_run_run
    proj_not_imported_compile proj_bare_cross_dir_compile
    duplicate_package_path_compile ncc_no_o_output_cwd
    proj_two_scripts_compile proj_imports_script_lib
    proj_imports_script_compile proj_imports_script_run
    proj_imports_lib_stmts_compile
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

# In-process VM tests resolve the stdlib native modules through the
# loader's build-tree fallback (NativeLibraryLoader walks up from the test
# exe to <build>/native/<config>, the single authoritative module output).
# No copies land in tests/<config> anymore; the fallback, not an exe-dir
# copy, is what satisfies native lookups the host never asked for.


# --- Embedding API tests (spec: 2026-10-08-nlang-embedded-host-api) ---
add_executable(test_embed
    test_embed/test_embed.cpp
)
#Marshalling.h is a src/embed internal header: the marshalling layer has
#no public-API observability without a VM, so this test reaches it
#directly (test_nculinker PRIVATE-includes src/vm the same way).
target_include_directories(test_embed
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/embed
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_embed
    PRIVATE
        nlang_embed
)
add_test(NAME embed_value_tests COMMAND test_embed)

#Embedding fixtures: real ncc output (build-time, mylib pattern). The
#value/hostfns/containers fixtures are pure computation; io_demo imports
#the stdlib io module and pulls in the nlang_stdlib_package edge below.
set(embed_fixture_dir ${CMAKE_BINARY_DIR}/embed_fixtures)
add_custom_target(nlang_embed_fixtures ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${embed_fixture_dir}
    COMMAND $<TARGET_FILE:ncc> build
        ${PROJECT_SOURCE_DIR}/tests/fixtures/embed/values_demo.n
        -o ${embed_fixture_dir}/values_demo.ncu
    COMMAND $<TARGET_FILE:ncc> build
        ${PROJECT_SOURCE_DIR}/tests/fixtures/embed/hostfns.n
        -o ${embed_fixture_dir}/hostfns.ncu
    #Library form of the host-function fixture (no main): importing
    #tests use it — Layer 6 forbids importing a main-defining module,
    #while the v1 load() entry contract keeps main() in hostfns.n.
    COMMAND $<TARGET_FILE:ncc> build
        ${PROJECT_SOURCE_DIR}/tests/fixtures/embed/hostfns_lib.n
        -o ${embed_fixture_dir}/hostfns_lib.ncu
    COMMAND $<TARGET_FILE:ncc> build
        ${PROJECT_SOURCE_DIR}/tests/fixtures/embed/containers_demo.n
        -o ${embed_fixture_dir}/containers_demo.ncu
    COMMAND $<TARGET_FILE:ncc> build
        ${PROJECT_SOURCE_DIR}/tests/fixtures/embed/io_demo.n
        -o ${embed_fixture_dir}/io_demo.ncu
    COMMAND $<TARGET_FILE:ncc> build
        ${PROJECT_SOURCE_DIR}/tests/fixtures/embed/math.n
        -o ${embed_fixture_dir}/math.ncu
    DEPENDS ncc
    VERBATIM)
#io_demo imports the stdlib io module: ncc resolves it against the
#build-tree stdlib copy, so the package must be packed before this
#target builds (fresh-tree targeted builds included).
add_dependencies(nlang_embed_fixtures nlang_stdlib_package)

add_executable(test_embed_vm
    test_embed/test_embed_vm.cpp
)
target_include_directories(test_embed_vm
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/src/embed
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_embed_vm
    PRIVATE
        nlang_vm
        nlang_runtime
        nlang_embed
)
target_compile_definitions(test_embed_vm
    PRIVATE EMBED_VALUES_NCU="${embed_fixture_dir}/values_demo.ncu")
add_dependencies(test_embed_vm nlang_embed_fixtures)
add_test(NAME embed_vm_tests COMMAND test_embed_vm)

add_executable(test_embed_api
    test_embed/test_embed_api.cpp
)
target_link_libraries(test_embed_api
    PRIVATE
        nlang_embed
)
target_compile_definitions(test_embed_api
    PRIVATE
        EMBED_VALUES_NCU="${embed_fixture_dir}/values_demo.ncu"
        EMBED_HOSTFNS_NCU="${embed_fixture_dir}/hostfns.ncu"
        EMBED_CONTAINERS_NCU="${embed_fixture_dir}/containers_demo.ncu"
        EMBED_IO_NCU="${embed_fixture_dir}/io_demo.ncu"
        EMBED_MATH_NCU="${embed_fixture_dir}/math.ncu"
        NLANG_TEST_STDLIB_DIR="${NLANG_TEST_STDLIB_DIR}")
add_dependencies(test_embed_api nlang_embed_fixtures)
add_test(NAME embed_api_tests COMMAND test_embed_api)

#Embedding source-entry tests (spec: 2026-10-15-embedding-source-entry).
#Deployed-stdlib fixture: one host-specified directory in the full
#deployment shape -- stdlib declaration sources + stdlib.npkg + native
#DLLs -- so import discovery, closure loading and native resolution all
#resolve from addImportDir dirs (the ONE-list contract). No DLL copies
#land next to the test exes anymore (the copy target is gone), but the
#loader's build-tree fallback still rescues a load if the
#AddNativeSearchDir wiring regresses -- the wiring is exercised (the
#happy path proves host dirs resolve first), not absence-probed.
set(embed_deploy_dir ${CMAKE_BINARY_DIR}/embed_deploy)
add_custom_target(nlang_embed_deploy ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${NLANG_TEST_STDLIB_DIR} ${embed_deploy_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:nlang_io>
                $<TARGET_FILE:nlang_math>
                $<TARGET_FILE:nlang_fs>
                ${embed_deploy_dir}
    DEPENDS nlang_stdlib_package nlang_io nlang_math nlang_fs
    VERBATIM)

add_executable(test_embed_source
    test_embed/test_embed_source.cpp
)
target_include_directories(test_embed_source
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/embed
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_embed_source
    PRIVATE
        nlang_embed
)
target_compile_definitions(test_embed_source
    PRIVATE
        EMBED_DEPLOY_DIR="${embed_deploy_dir}"
        EMBED_FIXTURE_DIR="${embed_fixture_dir}"
        EMBED_FIXTURE_SRC_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/embed"
        EMBED_IO_NCU="${embed_fixture_dir}/io_demo.ncu")
add_dependencies(test_embed_source nlang_embed_fixtures nlang_embed_deploy)
add_test(NAME embed_source_tests COMMAND test_embed_source)
#House rule: every test stays under a 3-minute ceiling (a hang must not
#block the full gate for 25 minutes).
set_tests_properties(embed_source_tests PROPERTIES TIMEOUT 180)

#Embedding example smoke: run a stdlib-importing script through
#examples/embed_host and assert the redirected output (Task 11).
if(Python3_FOUND)
    add_test(NAME embed_host_smoke
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/e2e/run_embed_host.py
            $<TARGET_FILE:embed_host>
            $<TARGET_FILE:ncc>
            ${NLANG_TEST_STDLIB_DIR}/stdlib.npkg
            $<TARGET_FILE:nlang_io>)
    set_tests_properties(embed_host_smoke PROPERTIES TIMEOUT 120)
endif()


#Same-name same-arity type overloads across user units (the tensor
#elementwise shape in plain project code): the shadow-registered
#producer index single-selects each call's own record. The fixture
#self-checks all three overloads before printing 42.
add_test(NAME proj_user_overload_compile
    COMMAND ncc build -p
        ${PROJECT_SOURCE_DIR}/tests/e2e/proj_user_overload/proj_user_overload.nproj
        -o ${CMAKE_BINARY_DIR}/e2e_out/proj_user_overload.npkg)
add_test(NAME proj_user_overload_run
    COMMAND nvm ${CMAKE_BINARY_DIR}/e2e_out/proj_user_overload.npkg)
set_tests_properties(proj_user_overload_run PROPERTIES
    DEPENDS proj_user_overload_compile
    PASS_REGULAR_EXPRESSION "42")
