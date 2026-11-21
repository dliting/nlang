# gate_time_timeouts.py -- guard five's axis: whether the TIMEOUT
# properties registered in ctest agree with the watchdog caps the
# reference table derives for this tree's own config-key set and
# configure-time work factor (docs/dev/gate_timing_design.md, D-2/D-8).
# show_only_json / read_timeouts read the registered side (ctest
# --show-only), threshold_inputs reads the emitter's own inputs back
# from the generated thresholds header, expected_watchdogs replays
# those inputs against the current table (caps_values is the single
# formula site, D-5) and check_timeouts states the agreement verdict.
# The freshness predicate, the site lookup and the CLI dispatch stay in
# the sibling gate_time_check.py; the budget guards live in
# gate_time_guards.py. Stdlib only; run with -I.

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_table import caps_values

# Guard five (D-2/D-8): the registered ctest TIMEOUT properties must
# equal the watchdog caps the table derives for this tree's own
# config-key set and configure-time work factor. Nothing about the tree
# is assumed: the caps are read from ctest --show-only, and the key set
# plus the factor come from the gate_time_thresholds.cmake header that
# very configure wrote, so the equation replays the emitter's own inputs
# against the current table.
TIMEOUT_MATCH_TOL_S = 0.1


def show_only_json(tree):
    """Raw `ctest --show-only=json-v1` document of a configured tree
    (the same -C Debug invocation guard one's enumeration uses:
    harmless on single-config generators, required on multi-config
    ones)."""
    proc = subprocess.run(
        ["ctest", "--test-dir", str(tree), "-C", "Debug",
         "--show-only=json-v1"],
        capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise ValueError("ctest --show-only of %s failed (rc=%d): is the "
                         "tree configured?" % (tree, proc.returncode))
    return proc.stdout


def read_timeouts(show_only_text):
    """Registered TIMEOUT per entry from a ctest --show-only=json-v1
    document: {name: seconds or None}. None means the entry carries no
    TIMEOUT property -- the row-less shape whose cap belongs to the
    leg's flat --timeout fallback (the ownership split
    tools/gate_tier.sh states)."""
    data = json.loads(show_only_text)
    timeouts = {}
    for test in data.get("tests", []):
        value = None
        for prop in test.get("properties", []):
            if prop.get("name") == "TIMEOUT":
                value = float(prop["value"])
                break
        timeouts[test["name"]] = value
    return timeouts


def threshold_inputs(text):
    """The generated gate_time_thresholds.cmake header: the config-key
    set and the configure-time work factor that produced this tree's
    caps (the `# configs:` / `# work_factor:` lines emit_cmake writes).
    Guard five replays exactly those inputs against the current table;
    a missing header means the file did not come from the cap emitter."""
    configs = None
    work_factor = None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("# configs:"):
            configs = stripped[len("# configs:"):].split()
        elif stripped.startswith("# work_factor:"):
            raw = stripped[len("# work_factor:"):].strip()
            try:
                work_factor = float(raw)
            except ValueError:
                raise ValueError("thresholds file carries a non-numeric "
                                 "work factor %r" % raw)
    if not configs:
        raise ValueError("thresholds file carries no '# configs:' header: "
                         "was this tree configured with the cap emitter?")
    if work_factor is None:
        raise ValueError("thresholds file carries no '# work_factor:' "
                         "header")
    return configs, work_factor


def expected_watchdogs(rows, configs, work_factor):
    """Guard five's equation right-hand side, {entry: watchdog cap}:
    caps_values (the single formula site, D-5) under each config key the
    tree was configured with, the largest over the keys carrying a row
    (one TIMEOUT property cannot switch with ctest -C, so the widest key
    wins -- the same max rule emit_cmake applied when it wrote the
    file). The machine scale plays no part: the watchdog stays
    machine-free (D-5)."""
    keys = set(configs)
    expected = {}
    for row in rows:
        if row.config not in keys or row.entry == "default":
            continue
        _budget, watchdog = caps_values(row.ref_s, work_factor, 1.0)
        prev = expected.get(row.entry)
        if prev is None or watchdog > prev:
            expected[row.entry] = watchdog
    return expected


def check_timeouts(registered, expected, fresh, site_of=None):
    """Guard five: every enumerated entry's registered TIMEOUT must
    equal the table-derived watchdog cap -- floored entries check
    against the floored value, the bare product would misjudge them red
    (D-1's full formula) -- within TIMEOUT_MATCH_TOL_S because ctest's
    json channel may round the cap float it displays. A mismatch means
    the registered value was not derived from the current table under
    this tree's key set, and the wording splits by configure freshness
    (D-2's disposition split): a stale configure still holds the
    previous table's caps, so ONE finding says to re-run configure
    instead of naming every moved entry; a fresh configure means a
    hand-written TIMEOUT property was set after the wrapper installed
    the table cap, so each entry is named (with its line in
    tests/CMakeLists.txt when site_of can point at one -- the property
    delete is the whole fix). Neither shape may read as a hang: the
    hang wording is the timed leg's TIMEOUT_WATCHDOG verdict. Returns
    (ok, findings)."""
    mismatches = []
    for name in sorted(registered):
        got = registered[name]
        want = expected.get(name)
        if got is None and want is None:
            continue
        if (got is None or want is None
                or abs(got - want) > TIMEOUT_MATCH_TOL_S):
            mismatches.append((name, got, want))
    if not mismatches:
        return True, []
    if not fresh:
        return False, ["TIMEOUTS: registered caps and the current table "
                       "disagree after the last configure; re-run "
                       "configure on this tree (only CTestTestfile.cmake "
                       "regenerates, nothing recompiles)"]
    findings = []
    for name, got, want in mismatches:
        site = site_of(name) if site_of is not None else ""
        findings.append(
            "TIMEOUT %s registered=%s want=%s: the configure is fresh, "
            "so a hand-written TIMEOUT property survived after the "
            "table-derived cap -- delete it%s"
            % (name, "none" if got is None else "%.1f" % got,
               "none" if want is None else "%.1f" % want,
               "; site " + site if site else ""))
    return False, findings
