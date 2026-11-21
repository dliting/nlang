# gate_time_keys.cmake -- the single decision site for a tree's gate
# timing config keys and the configure-time TIMEOUT cap generation
# (gate timing design D-1/D-2). tests/CMakeLists.txt includes it once
# Python3 is found; the gate_time_selftest.sh stub fixture includes the
# very same file after setting the knobs below, so the predicate has one
# production definition and no mirror that can drift from it (R-20).
#
# Knobs (every default self-locates from this file's directory, so the
# stub project needs no path rewiring):
#   NLANG_GATE_PYTHON   interpreter running the checker CLI
#   NLANG_GATE_TOOL     gate_time_check.py path
#   NLANG_GATE_TABLE    committed reference table
#   NLANG_GATE_OUT      generated thresholds file (written, then included)
# Inputs read as-is: CMAKE_CXX_FLAGS (the sanitizer predicate),
# NLANG_BUILD_IDE and the NLANG_TEST_WORK_FACTOR environment variable.
# Outputs: NLANG_GATE_TIMEOUT_KEYS (the tree's config-key set, read by
# the qtest wrapper wiring) and one GATE_TIMEOUT_<entry> variable per
# row-holding entry from the generated file.
#
# The config-key set mirrors the legs that will judge this tree, so an
# entry's cap derives from the very reference seconds its leg compares
# against. Branch priority (one site, read top to bottom): the sanitizer
# predicate wins over everything -- an instrumented tree is swept by the
# ctest-ASan leg alone, and that column's rows already carry the
# sanitizer cost, so the sanitizer tree takes the single-element key set
# {ctest-ASan}. Every other tree joins the ctest-Release column on top
# of its enumeration key (ctest-IDE when the IDE is built, ctest-Debug
# otherwise), unconditionally and regardless of NLANG_PERF_TESTS: the
# branches must read the tree's shape (instrumented or plain), never its
# identity as some leg's sweep tree, so a cap never drifts when a
# different tree starts sweeping the Release-side entries (design D-2;
# guard five judges the same key sets).
# The machine scale (guard four) plays no part here: it reaches test
# processes only through the leg environment (D-4), so the generated file
# holds no NLANG_GATE_MACHINE_SCALE note and a stale scale can never
# stale-out a cap. NLANG_TEST_WORK_FACTOR (D-8) scales every cap when set;
# when it is unset the --work-factor flag must be omitted entirely -- an
# empty value would fail the tool's parse rather than mean "1" -- so the
# factor rides a conditional branch instead of a substitution; an empty
# string is treated as unset.

if(NOT DEFINED NLANG_GATE_PYTHON)
    set(NLANG_GATE_PYTHON "${Python3_EXECUTABLE}")
endif()
if(NOT DEFINED NLANG_GATE_TOOL)
    set(NLANG_GATE_TOOL
        "${CMAKE_CURRENT_LIST_DIR}/../tools/gate_time_check.py")
endif()
if(NOT DEFINED NLANG_GATE_TABLE)
    set(NLANG_GATE_TABLE
        "${CMAKE_CURRENT_LIST_DIR}/gate_time_reference.txt")
endif()
if(NOT DEFINED NLANG_GATE_OUT)
    set(NLANG_GATE_OUT "${CMAKE_BINARY_DIR}/gate_time_thresholds.cmake")
endif()

if(CMAKE_CXX_FLAGS MATCHES "fsanitize=address")
    set(NLANG_GATE_TIMEOUT_KEYS "ctest-ASan")
else()
    if(NLANG_BUILD_IDE)
        set(NLANG_GATE_TIMEOUT_KEYS "ctest-IDE")
    else()
        set(NLANG_GATE_TIMEOUT_KEYS "ctest-Debug")
    endif()
    list(APPEND NLANG_GATE_TIMEOUT_KEYS "ctest-Release")
endif()
set(NLANG_GATE_TIMEOUT_CMD
    "${NLANG_GATE_PYTHON}" -I
    "${NLANG_GATE_TOOL}" emit-cmake
    --table "${NLANG_GATE_TABLE}"
    --out "${NLANG_GATE_OUT}")
foreach(nlang_gate_key IN LISTS NLANG_GATE_TIMEOUT_KEYS)
    list(APPEND NLANG_GATE_TIMEOUT_CMD --config "${nlang_gate_key}")
endforeach()
if(DEFINED ENV{NLANG_TEST_WORK_FACTOR}
        AND NOT "$ENV{NLANG_TEST_WORK_FACTOR}" STREQUAL "")
    list(APPEND NLANG_GATE_TIMEOUT_CMD
        --work-factor "$ENV{NLANG_TEST_WORK_FACTOR}")
endif()
execute_process(COMMAND ${NLANG_GATE_TIMEOUT_CMD}
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    RESULT_VARIABLE NLANG_GATE_TIMEOUT_RC
    OUTPUT_VARIABLE NLANG_GATE_TIMEOUT_OUT
    ERROR_VARIABLE NLANG_GATE_TIMEOUT_ERR)
if(NOT NLANG_GATE_TIMEOUT_RC EQUAL 0)
    # D-8: an invalid factor or malformed table line stops the
    # configure with the tool's own words quoted in front of the
    # user -- silently mis-derived caps would otherwise mis-gate
    # every later leg of this tree.
    message(FATAL_ERROR
        "gate_time_check.py emit-cmake failed (rc=${NLANG_GATE_TIMEOUT_RC},"
        " keys: ${NLANG_GATE_TIMEOUT_KEYS})\n"
        "${NLANG_GATE_TIMEOUT_OUT}${NLANG_GATE_TIMEOUT_ERR}")
endif()
include("${NLANG_GATE_OUT}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${NLANG_GATE_TABLE}")
# The local overlay table (gate_time_reference.local.txt beside the
# committed table, folded into caps() by _effective_rows) is just as
# much a cap input as the committed table, so editing it must force a
# reconfigure too. It is a local file that never enters the repository,
# so its absence is the normal state and the dependency is guarded.
get_filename_component(nlang_gate_table_dir "${NLANG_GATE_TABLE}" DIRECTORY)
set(nlang_gate_reference_local
    "${nlang_gate_table_dir}/gate_time_reference.local.txt")
if(EXISTS "${nlang_gate_reference_local}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${nlang_gate_reference_local}")
endif()
