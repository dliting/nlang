#!/usr/bin/env bash
# gate_time_selftest.sh — behaviour tests for the configure-time derived
# ctest TIMEOUT watchdog caps (tools/gate_time_check.py emit-cmake plus the
# generation block and add_test wrapper in tests/CMakeLists.txt).
#
# The fixture is a scratch CMake project that runs the real generator over a
# stub reference table. The two production code shapes (the config-key branch
# and the wrapper's cap lookup) are mirrored verbatim here so the production
# file can stay one inline copy; the stub project registers only stub entries,
# so nothing compiles and no build tree is needed, and the audit reads the
# timeouts from `ctest --show-only=json-v1` — no entry is executed by the
# audit, so machine load cannot turn a correct cap red.
#
# What gets pinned (one case per line; gate timing design D-1/D-2/D-4/D-8):
#   base-caps            the D-1 watchdog cap max(ref_s * 10, 120) per config
#                        row (a 100 s row caps past the floor, a 5 s row is
#                        held by the floor), a dotted entry gets its cap
#                        through the sanitized variable name, and a row-less
#                        entry gets no TIMEOUT property at all because its
#                        cap belongs to the leg's flat --timeout fallback
#                        (the ownership split stated in tools/gate_tier.sh).
#   machine-scale-absent the generated file never mentions the machine scale:
#                        the scalar reaches test processes only through the
#                        leg environment (D-4), so no configure bakes it in.
#   inherit-probe        a stub entry that reads an environment variable
#                        purely by inheritance sees its value under
#                        `VAR=value ctest -R ...` — that inheritance is the
#                        channel the machine scale (guard four) travels on,
#                        and nothing else supplies it.
#   work-factor-two      NLANG_TEST_WORK_FACTOR=2 at configure time doubles
#                        every row-backed cap while the 120 s floor holds.
#   perf-on-release-key  NLANG_PERF_TESTS=ON joins the ctest-Release key, so
#                        the perf entry gets a cap exactly when its entries
#                        are enumerated.
#   asan-branch-priority -fsanitize=address narrows the key set to ctest-ASan
#                        alone: that column's (deliberately lower) cap
#                        replaces the Debug row's instead of maxing with it,
#                        and Debug-only rows lose their properties.
#   ide-key              NLANG_BUILD_IDE=ON selects the ctest-IDE enumeration
#                        key instead of ctest-Debug.
#   work-factor-low      D-8: NLANG_TEST_WORK_FACTOR=0.5 must fail the
#                        configure and quote the tool's "work factor" wording,
#                        so a bad factor can never leave the tree with
#                        silently mis-derived caps.
#
# Toolchain paths arrive in the environment: the registration in
# tests/CMakeLists.txt passes NLANG_GATE_CMAKE, NLANG_GATE_CTEST and
# NLANG_GATE_PYTHON through nlang_test_env, and an interactive run falls back
# to the PATH lookups. A missing cmake, ctest or python prints a SKIP line
# and passes — the entry is registered only when the configuring CMake found
# all three, but carrying this script to a machine without them must not
# read as a false red.
#
# Scratch lives in $NLANG_TEST_SCRATCH_ROOT (the scratch root the add_test
# wrapper installs into every test environment) or a mktemp directory, under
# one fresh per-run directory. The stub table is named stub_table.txt rather
# than gate_time_reference.txt so the tool's local overlay
# (gate_time_reference.local.txt beside a table) can never reach the stub
# readings by accident.

set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
EMIT_TOOL="$REPO/tools/gate_time_check.py"

failures=0
pass() { printf 'selftest: PASS %s\n' "$1"; }
fail() { printf 'selftest: FAIL %s: %s\n' "$1" "$2"; failures=$((failures + 1)); }

CMAKE=${NLANG_GATE_CMAKE:-$(command -v cmake || true)}
CTEST=${NLANG_GATE_CTEST:-$(command -v ctest || true)}
PYTHON=${NLANG_GATE_PYTHON:-$(command -v python || command -v python3 || true)}
if [ -z "$CMAKE" ] || [ -z "$CTEST" ] || [ -z "$PYTHON" ]; then
    echo "selftest: SKIP toolchain: cmake=[$CMAKE] ctest=[$CTEST] python=[$PYTHON]"
    echo "SELFTEST: PASS"
    exit 0
fi

SCRATCH_ROOT=${NLANG_TEST_SCRATCH_ROOT:-$(mktemp -d)}
WS="$SCRATCH_ROOT/gate_time_selftest_$$${RANDOM}"
if ! mkdir -p "$WS/src" "$WS/build" "$WS/log"; then
    echo "selftest: FAIL workspace: cannot create $WS"
    echo "SELFTEST: FAIL n=1"
    exit 1
fi
# NLANG_GATE_TIME_KEEP_WS=1 leaves the scratch project behind so a failure's
# configure logs can be inspected from the shell (the ctest entry never sets
# it, so normal runs stay self-cleaning).
trap '[ -n "${NLANG_GATE_TIME_KEEP_WS:-}" ] || rm -rf "$WS"' EXIT

# --- fixtures ---------------------------------------------------------------
# Stub rows in the committed table's whitespace grammar (six columns minimum;
# the numbers are fixture values, not measurements). stub_big_probe carries a
# lower ctest-ASan row than its ctest-Debug row and a distinct ctest-IDE row:
# only replacement-by-one-key distinguishes those branches from a key set
# that joined the columns and took the max.

cat > "$WS/src/stub_table.txt" <<'STUB_TABLE_EOF'
# Stub reference table for tools/gate_time_selftest.sh.
ctest-Debug   stub_big_probe       100.00  1  常规  ctest-junit-serial
ctest-ASan    stub_big_probe        50.00  1  常规  ctest-junit-serial
ctest-IDE     stub_big_probe        80.00  1  常规  ctest-junit-serial
ctest-Debug   stub_small_probe       5.00  1  常规  ctest-junit-serial
ctest-Debug   stub.dash-name-probe  12.50  1  常规  ctest-junit-serial
ctest-Release stub_release_only    300.00  1  性能  ctest-junit-serial
STUB_TABLE_EOF

# The two mirrored production shapes below are copied verbatim from
# tests/CMakeLists.txt (the nlang_test_env merge helper, the add_test
# wrapper's cap lookup and scratch environment, and the generation block's
# key-set branch, command assembly, result check, include and
# CMAKE_CONFIGURE_DEPENDS). Paths and the interpreter arrive through -D
# cache variables, never literals, so the same text serves any checkout;
# production wraps the block in if(Python3_FOUND) while the fixture takes
# its interpreter from GATE_STUB_PYTHON instead. The inherit probe's command
# is one Python expression without a semicolon: the wrapper's
# _add_test(${ARGV}) forwarding re-splits a semicolon item into two
# arguments, and that split must not be what the probe accidentally pins.

cat > "$WS/src/CMakeLists.txt" <<'STUB_CMAKE_EOF'
cmake_minimum_required(VERSION 3.16)
project(nlang_gate_time_selftest_stub NONE)
enable_testing()

option(NLANG_PERF_TESTS "fixture mirror of the production perf switch" OFF)
option(NLANG_BUILD_IDE "fixture mirror of the production IDE switch" OFF)

set(NLANG_TEST_SCRATCH_ROOT "${CMAKE_BINARY_DIR}/scratch")
file(MAKE_DIRECTORY "${NLANG_TEST_SCRATCH_ROOT}")

function(nlang_test_env name)
    get_property(merged TEST ${name} PROPERTY ENVIRONMENT)
    foreach(pair IN LISTS ARGN)
        string(FIND "${pair}" "=" sep_pos)
        if(sep_pos LESS 0)
            message(FATAL_ERROR
                "nlang_test_env(${name}): ${pair} is not VAR=value")
        endif()
        string(SUBSTRING "${pair}" 0 ${sep_pos} pair_var)
        foreach(item IN LISTS merged)
            if(item MATCHES "^${pair_var}=")
                list(REMOVE_ITEM merged "${item}")
            endif()
        endforeach()
        list(APPEND merged "${pair}")
    endforeach()
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${merged}")
endfunction()
function(add_test)
    _add_test(${ARGV})
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" nlang_gate_timeout_key
        "${ARGV1}")
    if(DEFINED GATE_TIMEOUT_${nlang_gate_timeout_key})
        set_tests_properties(${ARGV1} PROPERTIES
            TIMEOUT "${GATE_TIMEOUT_${nlang_gate_timeout_key}}")
    endif()
    nlang_test_env(${ARGV1}
        TMP=${NLANG_TEST_SCRATCH_ROOT} TEMP=${NLANG_TEST_SCRATCH_ROOT})
endfunction()

if(CMAKE_CXX_FLAGS MATCHES "fsanitize=address")
    set(NLANG_GATE_TIMEOUT_KEYS "ctest-ASan")
else()
    if(NLANG_BUILD_IDE)
        set(NLANG_GATE_TIMEOUT_KEYS "ctest-IDE")
    else()
        set(NLANG_GATE_TIMEOUT_KEYS "ctest-Debug")
    endif()
    if(NLANG_PERF_TESTS)
        list(APPEND NLANG_GATE_TIMEOUT_KEYS "ctest-Release")
    endif()
endif()
set(NLANG_GATE_TIMEOUT_CMD
    "${GATE_STUB_PYTHON}" -I "${GATE_STUB_TOOL}" emit-cmake
    --table "${GATE_STUB_TABLE}"
    --out "${CMAKE_BINARY_DIR}/gate_time_thresholds.cmake")
foreach(nlang_gate_key IN LISTS NLANG_GATE_TIMEOUT_KEYS)
    list(APPEND NLANG_GATE_TIMEOUT_CMD --config "${nlang_gate_key}")
endforeach()
if(DEFINED ENV{NLANG_TEST_WORK_FACTOR})
    list(APPEND NLANG_GATE_TIMEOUT_CMD
        --work-factor "$ENV{NLANG_TEST_WORK_FACTOR}")
endif()
execute_process(COMMAND ${NLANG_GATE_TIMEOUT_CMD}
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    RESULT_VARIABLE NLANG_GATE_TIMEOUT_RC
    OUTPUT_VARIABLE NLANG_GATE_TIMEOUT_OUT
    ERROR_VARIABLE NLANG_GATE_TIMEOUT_ERR)
if(NOT NLANG_GATE_TIMEOUT_RC EQUAL 0)
    message(FATAL_ERROR
        "gate_time_check.py emit-cmake failed (rc=${NLANG_GATE_TIMEOUT_RC},"
        " keys: ${NLANG_GATE_TIMEOUT_KEYS})\n"
        "${NLANG_GATE_TIMEOUT_OUT}${NLANG_GATE_TIMEOUT_ERR}")
endif()
include("${CMAKE_BINARY_DIR}/gate_time_thresholds.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${GATE_STUB_TABLE}")

foreach(stub_entry stub_big_probe stub_small_probe stub.dash-name-probe
        stub_release_only stub_no_row_probe)
    add_test(NAME ${stub_entry} COMMAND "${GATE_STUB_PYTHON}" -c "pass")
endforeach()
add_test(NAME stub_env_inherit_probe
    COMMAND "${GATE_STUB_PYTHON}" -c
    "print('INHERIT=' + __import__('os').environ.get('NLANG_T4_INHERIT_PROBE', 'MISSING'))")
STUB_CMAKE_EOF

# --- probes -----------------------------------------------------------------

# Reconfigure the fixture. The work factor reaches CMake only through the
# environment (production reads ENV{NLANG_TEST_WORK_FACTOR}), so the factor
# rides `env` here; NONE removes the variable explicitly so an ambient value
# from the calling shell can never lean on the "no --work-factor flag" pin.
configure_fixture() { # configure_fixture <log-stem> <factor|NONE> [extra cmake args]
    local log="$1" factor="$2"; shift 2
    if [ "$factor" = NONE ]; then
        env -u NLANG_TEST_WORK_FACTOR "$CMAKE" -S "$WS/src" -B "$WS/build" \
            "-DGATE_STUB_PYTHON=$PYTHON" "-DGATE_STUB_TOOL=$EMIT_TOOL" \
            "-DGATE_STUB_TABLE=$WS/src/stub_table.txt" \
            "$@" > "$WS/log/$log.configure.txt" 2>&1
    else
        env NLANG_TEST_WORK_FACTOR="$factor" "$CMAKE" -S "$WS/src" -B "$WS/build" \
            "-DGATE_STUB_PYTHON=$PYTHON" "-DGATE_STUB_TOOL=$EMIT_TOOL" \
            "-DGATE_STUB_TABLE=$WS/src/stub_table.txt" \
            "$@" > "$WS/log/$log.configure.txt" 2>&1
    fi
}

show_only() { # show_only <log-stem>  (ctest -C Debug: harmless on
              # single-config generators, required on multi-config ones)
    ( cd "$WS/build" && "$CTEST" -C Debug --show-only=json-v1 ) \
        > "$WS/log/$1.json" 2>&1
}

caps_table() { # caps_table <json-file> -> "<entry>\t<cap>" per capped entry
    "$PYTHON" -I -c 'import json, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
for test in doc["tests"]:
    for prop in test.get("properties", []):
        if prop["name"] == "TIMEOUT":
            print("%s\t%s" % (test["name"], prop["value"]))
            break' "$1"
}

# The cap lands in the generated file as the exact caps() float, and the
# json channel may round its display (measured: 1559.4000000000001 reads
# back as 1559.4), so the comparison carries a tenth-of-a-second tolerance.
expect_cap() { # expect_cap <case> <json-file> <entry> <expected|NONE>
    local got
    got=$(caps_table "$2" | awk -F '\t' -v e="$3" '$1 == e { print $2 }')
    if [ "$4" = NONE ]; then
        [ -z "$got" ] || fail "$1" "$3 carries TIMEOUT $got, want no property"
    elif [ -z "$got" ]; then
        fail "$1" "$3 has no TIMEOUT, want $4"
    elif ! awk -v g="$got" -v w="$4" \
            'BEGIN { d = g - w; if (d < 0) d = -d; exit !(d < 0.1) }'; then
        fail "$1" "$3 timeout $got, want $4"
    fi
}

tail_of() { tail -n 5 "$1" | tr '\n' ';'; }

# --- cases ------------------------------------------------------------------

if ! configure_fixture base NONE; then
    fail base-caps "base configure failed: [$(tail_of "$WS/log/base.configure.txt")]"
    echo "SELFTEST: FAIL n=$failures"
    exit 1
fi
show_only base
before=$failures
expect_cap base-caps "$WS/log/base.json" stub_big_probe 1000.0
expect_cap base-caps "$WS/log/base.json" stub_small_probe 120.0
expect_cap base-caps "$WS/log/base.json" stub.dash-name-probe 125.0
expect_cap base-caps "$WS/log/base.json" stub_release_only NONE
expect_cap base-caps "$WS/log/base.json" stub_no_row_probe NONE
expect_cap base-caps "$WS/log/base.json" stub_env_inherit_probe NONE
[ "$failures" -eq "$before" ] && pass base-caps

before=$failures
if grep -qi "machine scale" "$WS/build/gate_time_thresholds.cmake"; then
    fail machine-scale-absent "the generated file mentions the machine scale"
fi
[ "$failures" -eq "$before" ] && pass machine-scale-absent

before=$failures
# -V because the assertion reads the child's stdout: without it ctest keeps
# a passing test's output to itself and the marker would never surface.
inherit_out=$( cd "$WS/build" && \
    NLANG_T4_INHERIT_PROBE=marker "$CTEST" -C Debug -V \
    -R '^stub_env_inherit_probe$' 2>&1 )
case "$inherit_out" in
    *INHERIT=marker*) pass inherit-probe ;;
    *) fail inherit-probe "the inherited variable never reached the test process: [$(printf '%s' "$inherit_out" | tr '\n' ';')]" ;;
esac

if ! configure_fixture factor2 2; then
    fail work-factor-two "factor-2 configure failed: [$(tail_of "$WS/log/factor2.configure.txt")]"
else
    show_only factor2
    before=$failures
    expect_cap work-factor-two "$WS/log/factor2.json" stub_big_probe 2000.0
    expect_cap work-factor-two "$WS/log/factor2.json" stub_small_probe 120.0
    expect_cap work-factor-two "$WS/log/factor2.json" stub.dash-name-probe 250.0
    [ "$failures" -eq "$before" ] && pass work-factor-two
fi

if ! configure_fixture perf NONE -DNLANG_PERF_TESTS=ON; then
    fail perf-on-release-key "perf-on configure failed: [$(tail_of "$WS/log/perf.configure.txt")]"
else
    show_only perf
    before=$failures
    expect_cap perf-on-release-key "$WS/log/perf.json" stub_release_only 3000.0
    expect_cap perf-on-release-key "$WS/log/perf.json" stub_big_probe 1000.0
    [ "$failures" -eq "$before" ] && pass perf-on-release-key
fi

if ! configure_fixture asan NONE -DNLANG_PERF_TESTS=OFF \
        -DCMAKE_CXX_FLAGS=-fsanitize=address; then
    fail asan-branch-priority "asan configure failed: [$(tail_of "$WS/log/asan.configure.txt")]"
else
    show_only asan
    before=$failures
    expect_cap asan-branch-priority "$WS/log/asan.json" stub_big_probe 500.0
    expect_cap asan-branch-priority "$WS/log/asan.json" stub_small_probe NONE
    expect_cap asan-branch-priority "$WS/log/asan.json" stub.dash-name-probe NONE
    expect_cap asan-branch-priority "$WS/log/asan.json" stub_release_only NONE
    [ "$failures" -eq "$before" ] && pass asan-branch-priority
fi

# -DCMAKE_CXX_FLAGS= clears the sanitizer flag the asan case left in the
# cache: the sanitizer predicate wins over the IDE branch, so without the
# reset this case would measure the ASan column again (measured: 500.0).
if ! configure_fixture ide NONE -DNLANG_BUILD_IDE=ON -DCMAKE_CXX_FLAGS=; then
    fail ide-key "ide configure failed: [$(tail_of "$WS/log/ide.configure.txt")]"
else
    show_only ide
    before=$failures
    expect_cap ide-key "$WS/log/ide.json" stub_big_probe 800.0
    expect_cap ide-key "$WS/log/ide.json" stub_small_probe NONE
    [ "$failures" -eq "$before" ] && pass ide-key
fi

# D-8 last: a failed FATAL_ERROR configure can leave the cache mid-write, and
# every reading above must come from a cleanly configured tree first.
before=$failures
if configure_fixture low 0.5; then
    fail work-factor-low "a configure with NLANG_TEST_WORK_FACTOR=0.5 succeeded"
elif ! grep -qi "work factor" "$WS/log/low.configure.txt"; then
    fail work-factor-low "the failed configure never quoted the tool's wording: [$(tail_of "$WS/log/low.configure.txt")]"
fi
[ "$failures" -eq "$before" ] && pass work-factor-low

if [ "$failures" -eq 0 ]; then
    echo "SELFTEST: PASS"
else
    echo "SELFTEST: FAIL n=$failures"
fi
exit "$failures"
