#!/usr/bin/env bash
# gate_time_selftest.sh — behaviour tests for the configure-time derived
# ctest TIMEOUT watchdog caps (tools/gate_time_check.py emit-cmake plus the
# generation block and add_test wrapper in tests/CMakeLists.txt).
#
# The fixture is a scratch CMake project that runs the real generator over a
# stub reference table. The config-key predicate, command assembly, result
# check, include and CMAKE_CONFIGURE_DEPENDS are not mirrored at all: the
# stub project includes the very production file
# tests/gate_time_keys.cmake (through -DGATE_STUB_KEYS) after pointing its
# knobs (NLANG_GATE_PYTHON/TOOL/TABLE) at the stub paths, so the predicate
# has one definition and no mirror that could drift from it. Only the two
# shapes tests/CMakeLists.txt itself keeps — the nlang_test_env merge helper
# and the add_test wrapper's cap lookup and scratch environment — are
# mirrored verbatim here. The stub project registers only stub entries, so
# nothing compiles and no build tree is needed, and the audit reads the
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
#                        The plain-tree branch also joins the ctest-Release
#                        column unconditionally (D-2), so the Release-only
#                        stub row is capped here with no perf switch in the
#                        fixture at all -- the shape the design calls for
#                        and a perf-gated join could never produce.
#   machine-scale-absent the generated file never mentions the machine scale:
#                        the scalar reaches test processes only through the
#                        leg environment (D-4), so no configure bakes it in.
#   timeouts-live        guard five green half: the fresh tree's registered
#                        caps replay green through check-timeouts against the
#                        very document ctest's own show-only printed.
#   timeouts-stale-wording a table edited behind the configure's back folds
#                        every drifted cap into ONE "re-run configure"
#                        finding — no per-entry edit list for caps that were
#                        never hand-written.
#   inherit-probe        a stub entry that reads an environment variable
#                        purely by inheritance sees its value under
#                        `VAR=value ctest -R ...` — that inheritance is the
#                        channel the machine scale (guard four) travels on,
#                        and nothing else supplies it.
#   rounds-selfreport    the D-8 loop end to end: nlang_test_env installs the
#                        round count, the stub entry multiplies it by the
#                        inherited work factor and self-reports rounds=<n>
#                        into the junit <system-out>, and check-budget passes
#                        when the report equals ref_work x factor and goes
#                        red naming both numbers at a factor the loop never
#                        saw.
#   work-factor-two      NLANG_TEST_WORK_FACTOR=2 at configure time doubles
#                        every row-backed cap while the 120 s floor holds.
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
#   work-factor-noninteger R-31: outright garbage (abc123) meets the same
#                        frozen positive-integer format with the same named
#                        wording -- the factor scales integer-counting
#                        consumers, so no spelling may parse differently
#                        per side instead of reaching the cap emitter.
#   slow-export          print-slow's two arms on the stub slow rows: without
#                        --keep-matched the config's slow names are exported,
#                        and with the flag only the matched name is subtracted
#                        while the rest survive -- the T0 wiring leans on
#                        exactly this split (R-53: a catch-all -R selector
#                        must never reach --keep-matched, or the conservative
#                        round empties its own -E; R-54: the export itself
#                        had no assertion before).
#
# Toolchain paths arrive in the environment: the registration in
# tests/CMakeLists.txt passes NLANG_GATE_CMAKE, NLANG_GATE_CTEST and
# NLANG_GATE_PYTHON through nlang_test_env, and an interactive run falls back
# to the PATH lookups. A missing cmake, ctest or python prints a SKIP line
# and passes — the entry is registered only when the configuring CMake found
# all three, but carrying this script to a machine without them must not
# read as a false red.
#
# Scratch lives in $NLANG_TEST_SCRATCH_ROOT as this script's own calling
# environment carries it — an interactive shell or a leg environment exports
# it, and unset falls back to a mktemp directory — under one fresh per-run
# directory. The inherit probe pins a different channel: it proves variables
# set on the ctest command line reach the test process by inheritance
# (tests/CMakeLists.txt installs only TMP/TEMP into every test environment),
# so it never relies on this variable. The tool's local overlay is found by
# a fixed name (gate_time_reference.local.txt) beside whichever table is
# loaded, so the stub table's name alone would not keep it out: what keeps
# it out is the scratch itself — the fixture table lives inside this run's
# freshly created workspace directory, which holds nothing until this
# script writes it.

set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
EMIT_TOOL="$REPO/tools/gate_time_check.py"

failures=0
pass() { printf 'selftest: PASS %s\n' "$1"; }
fail() { printf 'selftest: FAIL %s: %s\n' "$1" "$2"; failures=$((failures + 1)); }

CMAKE=${NLANG_GATE_CMAKE:-$(command -v cmake || true)}
CTEST=${NLANG_GATE_CTEST:-$(command -v ctest || true)}
PYTHON=${NLANG_GATE_PYTHON:-$(command -v python || command -v python3 || true)}
# An existence hit can still be an unlaunchable stub: Windows keeps an App
# Execution Alias named python on PATH even when no interpreter is installed
# (CreateProcess reports it as 9009), so the probe launches the interpreter
# it picked rather than trusting the name -- a machine without a real python
# must reach the SKIP line, not a configure failure.
if [ -n "$PYTHON" ] && ! "$PYTHON" -c pass >/dev/null 2>&1; then
    PYTHON=
fi
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
ctest-Debug   stub_rounds_probe     40.00  100  常规  ctest-junit-serial
ctest-Release stub_release_only    300.00  1  性能  ctest-junit-serial
# Slow-set rows for the slow-export probe below: both carry ref_s above
# SLOW_MIN_REF_S (60 s) AND a slow-class category, so the two names the
# probe asserts stay in the slow set under the current category rule and
# under the 60 s threshold rule planned next -- swapping the membership
# criterion cannot invalidate the probe. stub_slow_probe is the name
# --keep-matched subtracts; stub_slow_partner_probe must survive it
# (neither name contains the other, so the flag can only hit its own).
ctest-Debug   stub_slow_probe       90.00  1  慢功能  ctest-junit-serial
ctest-Debug   stub_slow_partner_probe  150.00  1  慢功能  ctest-junit-serial
STUB_TABLE_EOF

# The stub soak entry mirrors the round scaling of nd_resolve_stress_tests
# in tests/test_vm/test_nd_resolve.cpp: it multiplies NLANG_TEST_ROUNDS (the
# per-config count the registration installs through nlang_test_env) by the
# inherited NLANG_TEST_WORK_FACTOR (the environment axis), bounds both the
# same way, self-reports the product on stdout as the D-8 contract demands,
# and sleeps 0.01 s per round so the budget checker's wall clock tracks the
# loop (1 s base, 2 s at factor 2, inside the 80/160 s budgets either way).
cat > "$WS/src/rounds_probe.py" <<'ROUNDS_EOF'
import os
import time

def positive_env(name, fallback):
    try:
        value = int(os.environ.get(name, ""))
    except ValueError:
        return fallback
    return value if 1 <= value <= 1000000 else fallback

rounds = positive_env("NLANG_TEST_ROUNDS", 100)
try:
    factor = max(float(os.environ.get("NLANG_TEST_WORK_FACTOR") or "1"), 1.0)
except ValueError:
    factor = 1.0
rounds = int(rounds * factor)
print("rounds=%d" % rounds, flush=True)
time.sleep(0.01 * rounds)
ROUNDS_EOF

# The nlang_test_env merge helper and the add_test wrapper below are
# copied verbatim from tests/CMakeLists.txt, which keeps exactly those
# two shapes (the merge helper and the cap lookup with its scratch
# environment). Everything the old generation block owned — key-set
# branch, command assembly, result check, include and
# CMAKE_CONFIGURE_DEPENDS — is NOT mirrored: the fixture sets the
# production file's knobs from the GATE_STUB_* cache variables and then
# includes tests/gate_time_keys.cmake itself (GATE_STUB_KEYS), so the
# predicate under test is production's own text (R-20). Paths and the
# interpreter arrive through -D cache variables, never literals, so the
# same text serves any checkout; production resolves its interpreter
# from find_package(Python3) while the fixture passes it explicitly.
# The inherit probe's command is one Python expression without a
# semicolon: the wrapper's _add_test(${ARGV}) forwarding re-splits a
# semicolon item into two arguments, and that split must not be what the
# probe accidentally pins.

cat > "$WS/src/CMakeLists.txt" <<'STUB_CMAKE_EOF'
cmake_minimum_required(VERSION 3.16)
project(nlang_gate_time_selftest_stub NONE)
enable_testing()

option(NLANG_BUILD_IDE "fixture knob for the production IDE branch" OFF)

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

#Point the production file's knobs at the stub paths, then run its one
#definition (it self-locates nothing here: every knob arrives defined).
set(NLANG_GATE_PYTHON "${GATE_STUB_PYTHON}")
set(NLANG_GATE_TOOL "${GATE_STUB_TOOL}")
set(NLANG_GATE_TABLE "${GATE_STUB_TABLE}")
include("${GATE_STUB_KEYS}")

foreach(stub_entry stub_big_probe stub_small_probe stub.dash-name-probe
        stub_release_only stub_no_row_probe)
    add_test(NAME ${stub_entry} COMMAND "${GATE_STUB_PYTHON}" -c "pass")
endforeach()
add_test(NAME stub_env_inherit_probe
    COMMAND "${GATE_STUB_PYTHON}" -c
    "print('INHERIT=' + __import__('os').environ.get('NLANG_T4_INHERIT_PROBE', 'MISSING'))")

# The rounds registration mirrors nd_resolve_stress_tests in production's
# tests/CMakeLists.txt (there under an fsanitize=address branch): the soak
# round count enters the test environment through the merge helper, never
# by inheritance -- what the self-report cross-check holds the registration
# and the loop together with.
add_test(NAME stub_rounds_probe
    COMMAND "${GATE_STUB_PYTHON}" "${GATE_STUB_ROUNDS}")
nlang_test_env(stub_rounds_probe NLANG_TEST_ROUNDS=100)
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
            "-DGATE_STUB_KEYS=$REPO/tests/gate_time_keys.cmake" \
            "-DGATE_STUB_ROUNDS=$WS/src/rounds_probe.py" \
            "$@" > "$WS/log/$log.configure.txt" 2>&1
    else
        env NLANG_TEST_WORK_FACTOR="$factor" "$CMAKE" -S "$WS/src" -B "$WS/build" \
            "-DGATE_STUB_PYTHON=$PYTHON" "-DGATE_STUB_TOOL=$EMIT_TOOL" \
            "-DGATE_STUB_TABLE=$WS/src/stub_table.txt" \
            "-DGATE_STUB_KEYS=$REPO/tests/gate_time_keys.cmake" \
            "-DGATE_STUB_ROUNDS=$WS/src/rounds_probe.py" \
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

audit_timeouts() { # audit_timeouts <saved-show-only-doc> -> audit_rc/audit_out
    # The saved document feeds the audit on purpose: ctest's own PATH never
    # enters (the registered entry environment points at it through
    # NLANG_GATE_CTEST), and the audit reads exactly the registrations the
    # show-only case above already inspected.
    audit_out=$( "$PYTHON" -I "$EMIT_TOOL" check-timeouts --tree "$WS/build" \
        --table "$WS/src/stub_table.txt" --show-only "$1" 2>&1 )
    audit_rc=$?
}

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
expect_cap base-caps "$WS/log/base.json" stub_rounds_probe 400.0
expect_cap base-caps "$WS/log/base.json" stub_release_only 3000.0
expect_cap base-caps "$WS/log/base.json" stub_no_row_probe NONE
expect_cap base-caps "$WS/log/base.json" stub_env_inherit_probe NONE
[ "$failures" -eq "$before" ] && pass base-caps

before=$failures
if grep -qi "machine scale" "$WS/build/gate_time_thresholds.cmake"; then
    fail machine-scale-absent "the generated file mentions the machine scale"
fi
[ "$failures" -eq "$before" ] && pass machine-scale-absent

before=$failures
# Guard five green half, live: the caps this very configure registered
# replay green against the current table (its thresholds header states the
# config keys and work factor they were derived from, and the checker
# replays those rather than guessing from the shell).
audit_timeouts "$WS/log/base.json"
if [ "$audit_rc" -ne 0 ] \
        || ! printf '%s' "$audit_out" | grep -q "GATE gate_time_check: PASS"; then
    fail timeouts-live "the fresh tree did not audit green: rc=$audit_rc [$(printf '%s' "$audit_out" | tr '\n' ';')]"
fi
[ "$failures" -eq "$before" ] && pass timeouts-live

before=$failures
# Guard five stale half: editing the table behind the configure's back is
# the drift CMAKE_CONFIGURE_DEPENDS normally catches; while the tree
# predates the edit the right next step is one re-run configure (only
# CTestTestfile.cmake regenerates), not a hunt for hand-written properties
# that do not exist. 300.00 is stub_release_only's reference alone in the
# stub table, so the sed can stay bare, and the factor-2 reconfigure later
# in the run refreshes the stamp again.
sed -i 's/300\.00/301.00/' "$WS/src/stub_table.txt"
audit_timeouts "$WS/log/base.json"
if [ "$audit_rc" -eq 0 ]; then
    fail timeouts-stale-wording "the drifted table audited green"
elif ! printf '%s' "$audit_out" | grep -q "re-run configure"; then
    fail timeouts-stale-wording "the drifted table never named the remedy: [$(printf '%s' "$audit_out" | tr '\n' ';')]"
fi
case "$audit_out" in
    *"TIMEOUT stub_release_only registered="*)
        fail timeouts-stale-wording "the stale path walked entries instead of folding into one finding" ;;
esac
sed -i 's/301\.00/300.00/' "$WS/src/stub_table.txt"
[ "$failures" -eq "$before" ] && pass timeouts-stale-wording

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

before=$failures
# D-8 end to end on the stub soak entry: the registration installs
# NLANG_TEST_ROUNDS through nlang_test_env, the entry multiplies it by the
# inherited NLANG_TEST_WORK_FACTOR and self-reports the product on stdout
# (ctest --output-junit captures a passing entry's output into
# <system-out>), and check-budget holds the report to ref_work x the work
# factor it is run at. env -u on the base arm keeps an ambient factor from
# the caller's shell out of the base reading.
rounds_run() { # rounds_run <stem> <factor|NONE>  (the ctest round's rc)
    if [ "$2" = NONE ]; then
        ( cd "$WS/build" && env -u NLANG_TEST_WORK_FACTOR "$CTEST" -C Debug \
            --output-junit "$WS/log/$1.junit.xml" -R '^stub_rounds_probe$' ) \
            > "$WS/log/$1.run.txt" 2>&1
    else
        ( cd "$WS/build" && env NLANG_TEST_WORK_FACTOR="$2" "$CTEST" -C Debug \
            --output-junit "$WS/log/$1.junit.xml" -R '^stub_rounds_probe$' ) \
            > "$WS/log/$1.run.txt" 2>&1
    fi
}
rounds_audit() { # rounds_audit <stem> <work-factor> -> audit_rc/audit_out
    audit_out=$( "$PYTHON" -I "$EMIT_TOOL" check-budget --config ctest-Debug \
        --table "$WS/src/stub_table.txt" --junit "$WS/log/$1.junit.xml" \
        --work-factor "$2" 2>&1 )
    audit_rc=$?
}
rounds_run rounds1 NONE \
    || fail rounds-selfreport "the base ctest round failed: [$(tail_of "$WS/log/rounds1.run.txt")]"
rounds_audit rounds1 1.0
if [ "$audit_rc" -ne 0 ] \
        || ! printf '%s' "$audit_out" | grep -q "GATE gate_time_check: PASS"; then
    fail rounds-selfreport "the matching 100-round reading did not audit green: rc=$audit_rc [$(printf '%s' "$audit_out" | tr '\n' ';')]"
fi
if ! grep -q "<system-out>" "$WS/log/rounds1.junit.xml" \
        || ! grep -q "rounds=100" "$WS/log/rounds1.junit.xml"; then
    fail rounds-selfreport "the entry never self-reported rounds=100 into the junit document"
fi
rounds_run rounds2 2 \
    || fail rounds-selfreport "the factor-2 ctest round failed: [$(tail_of "$WS/log/rounds2.run.txt")]"
rounds_audit rounds2 2.0
if [ "$audit_rc" -ne 0 ]; then
    fail rounds-selfreport "the doubled 200-round reading at factor 2 did not audit green: rc=$audit_rc [$(printf '%s' "$audit_out" | tr '\n' ';')]"
fi
# The same document against a factor the loop never ran at: absence is the
# corpus's case, but report-against-priced-workload is what this contract
# exists to catch -- "the factor was set and the soak never read it".
rounds_audit rounds2 3.0
if [ "$audit_rc" -eq 0 ] \
        || ! printf '%s' "$audit_out" | grep -q \
            "SELFREPORT stub_rounds_probe: rounds=200, expected 300"; then
    fail rounds-selfreport "a factor the soak never ran at must red naming both numbers: rc=$audit_rc [$(printf '%s' "$audit_out" | tr '\n' ';')]"
fi
[ "$failures" -eq "$before" ] && pass rounds-selfreport

if ! configure_fixture factor2 2; then
    fail work-factor-two "factor-2 configure failed: [$(tail_of "$WS/log/factor2.configure.txt")]"
else
    show_only factor2
    before=$failures
    expect_cap work-factor-two "$WS/log/factor2.json" stub_big_probe 2000.0
    expect_cap work-factor-two "$WS/log/factor2.json" stub_small_probe 120.0
    expect_cap work-factor-two "$WS/log/factor2.json" stub.dash-name-probe 250.0
    expect_cap work-factor-two "$WS/log/factor2.json" stub_rounds_probe 800.0
    [ "$failures" -eq "$before" ] && pass work-factor-two
fi

if ! configure_fixture asan NONE \
        -DCMAKE_CXX_FLAGS=-fsanitize=address; then
    fail asan-branch-priority "asan configure failed: [$(tail_of "$WS/log/asan.configure.txt")]"
else
    show_only asan
    before=$failures
    expect_cap asan-branch-priority "$WS/log/asan.json" stub_big_probe 500.0
    expect_cap asan-branch-priority "$WS/log/asan.json" stub_small_probe NONE
    expect_cap asan-branch-priority "$WS/log/asan.json" stub.dash-name-probe NONE
    expect_cap asan-branch-priority "$WS/log/asan.json" stub_release_only NONE
    expect_cap asan-branch-priority "$WS/log/asan.json" stub_rounds_probe NONE
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
    expect_cap ide-key "$WS/log/ide.json" stub_rounds_probe NONE
    [ "$failures" -eq "$before" ] && pass ide-key
fi

# D-8 last: failed FATAL_ERROR configures can leave the cache mid-write, and
# every reading above must come from a cleanly configured tree first.
before=$failures
if configure_fixture low 0.5; then
    fail work-factor-low "a configure with NLANG_TEST_WORK_FACTOR=0.5 succeeded"
elif ! grep -qi "work factor" "$WS/log/low.configure.txt"; then
    fail work-factor-low "the failed configure never quoted the tool's wording: [$(tail_of "$WS/log/low.configure.txt")]"
fi
[ "$failures" -eq "$before" ] && pass work-factor-low

# R-31: garbage meets the same frozen positive-integer format as the 0.5
# above (the configure-time freeze in tests/gate_time_keys.cmake refuses
# every non-matching spelling with the same named wording), so no value
# can parse differently per side -- floats round, ints truncate.
before=$failures
if configure_fixture nonint abc123; then
    fail work-factor-noninteger "a configure with NLANG_TEST_WORK_FACTOR=abc123 succeeded"
elif ! grep -qi "work factor" "$WS/log/nonint.configure.txt"; then
    fail work-factor-noninteger "the failed configure never quoted the work-factor wording: [$(tail_of "$WS/log/nonint.configure.txt")]"
fi
[ "$failures" -eq "$before" ] && pass work-factor-noninteger

before=$failures
# R-54: print-slow is the single export of the slow set every scheduled
# ctest leg consumes, and its --keep-matched subtraction is the exact
# F-12 backfill the T0 wiring leans on. Both arms must be pinned here:
# without the flag the whole slow set is exported (the unmapped
# conservative T0 round runs that shape, and R-53 came from wiring
# handing its catch-all "." to the flag instead -- the exclusion then
# emptied and the slow entries ran in the fast round), and with the flag
# only the names it re.search-hits leave the set while the rest survive.
slow_base=$("$PYTHON" -I "$EMIT_TOOL" print-slow ctest-Debug \
    --table "$WS/src/stub_table.txt" 2>&1)
slow_base_rc=$?
slow_keep=$("$PYTHON" -I "$EMIT_TOOL" print-slow ctest-Debug \
    --table "$WS/src/stub_table.txt" --keep-matched stub_slow_probe 2>&1)
slow_keep_rc=$?
case "$slow_base" in
    *stub_slow_probe*) : ;;
    *) fail slow-export "print-slow without --keep-matched lost a slow name: [$(printf '%s' "$slow_base" | tr '\n' ';')]" ;;
esac
if [ "$slow_base_rc" -ne 0 ]; then
    fail slow-export "print-slow exited rc=$slow_base_rc: [$(printf '%s' "$slow_base" | tr '\n' ';')]"
fi
case "$slow_keep" in
    *stub_slow_probe*)
        fail slow-export "--keep-matched stub_slow_probe never subtracted it: [$(printf '%s' "$slow_keep" | tr '\n' ';')]" ;;
    *stub_slow_partner_probe*) : ;;
    *) fail slow-export "--keep-matched must subtract only the matched name, yet the surviving slow name vanished: [$(printf '%s' "$slow_keep" | tr '\n' ';')]" ;;
esac
if [ "$slow_keep_rc" -ne 0 ]; then
    fail slow-export "print-slow --keep-matched exited rc=$slow_keep_rc: [$(printf '%s' "$slow_keep" | tr '\n' ';')]"
fi
[ "$failures" -eq "$before" ] && pass slow-export

if [ "$failures" -eq 0 ]; then
    echo "SELFTEST: PASS"
else
    echo "SELFTEST: FAIL n=$failures"
fi
exit "$failures"
