# gate_time_guards.py -- the guard verdict bodies of the reference timing
# table: guard one (check_complete), guard two (check_orphans), guard
# seven (check_constants), guard three post-hoc with guard ten's form
# refusal (check_budget) and its D-8 self-report cross-check, guard five
# (check_timeouts plus its readers of the registered TIMEOUT properties
# and the generated thresholds header), plus the sentinel admission
# helpers behind guard four and the input readers that build the derived
# entry universes (docs/dev/gate_timing_design.md). Each check returns
# (ok, findings) -- or (findings, detail) for check_budget, whose detail
# lines are report lines, not findings. The row model and the caps
# formula live in gate_time_table.py; the sampling rounds and the CLI
# entry live in the sibling gate_time_measure / gate_time_check modules.
# Stdlib only; run with -I.

import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_table import (CONFIG_KEYS, CTEST_CONFIGS, DEFAULT_WORK_FACTOR,
                             E2E_CONFIGS, MANIFEST_SCOPES, MIN_CAP_S,
                             MIN_WATCHDOG_S, SAFETY_MULTIPLIER,
                             SCALE_BAND_MAX, SENTINELS_PER_CONFIG,
                             SENTINEL_MARK, WATCHDOG_MULTIPLIER,
                             caps_values, form_token, resolve_scoped_row)


def check_complete(rows, enums, manifests, perf_on=None):
    """Guard one: each derived entry needs a row in the config that
    consumes it (tree -> config key per D-2; e2e entries are covered by an
    entry row, a manifest-scope row or the config's default row), no row
    may point outside its config's derived set, and every participating
    config must carry SENTINELS_PER_CONFIG sentinel marks (D-4). Returns
    (ok, findings)."""
    findings = []
    entry_rows = set()
    defaults = set()
    marked = {}
    for row in rows:
        entry_rows.add((row.config, row.entry))
        if row.entry == "default":
            defaults.add(row.config)
        if row.sentinel == SENTINEL_MARK:
            marked.setdefault(row.config, set()).add(row.entry)
    required = {}
    for cfg in CTEST_CONFIGS:
        if enums.get(cfg) is not None:
            required[cfg] = set(enums[cfg])
        elif any(r.config == cfg for r in rows):
            findings.append("ENUM %s: no enumeration source, rows "
                            "unverifiable" % cfg)
    manifest_names = set((manifests or {}).get("manifest.txt") or ())
    examples_names = set((manifests or {}).get("examples_manifest.txt") or ())
    release_marked = marked.get("ctest-Release", set())
    if perf_on is not None or release_marked:
        required["ctest-Release"] = set(perf_on or ()) | release_marked
    for cfg in E2E_CONFIGS:
        names = set(manifest_names)
        if cfg == "e2e-Release":
            names |= examples_names
        required[cfg] = names
    for cfg in sorted(required):
        for name in sorted(required[cfg]):
            if (cfg, name) in entry_rows:
                continue
            if cfg in E2E_CONFIGS:
                if cfg in defaults:
                    continue
                if name in manifest_names and (cfg, "manifest.txt") in entry_rows:
                    continue
                if name in examples_names and (cfg, "examples_manifest.txt") in entry_rows:
                    continue
            findings.append("MISSING %s %s" % (cfg, name))
    for row in rows:
        if row.config in CTEST_CONFIGS:
            if row.entry == "default" or row.entry in MANIFEST_SCOPES:
                findings.append("EXTRA %s %s (ctest configs list entries "
                                "per row, no scope rows)" % (row.config,
                                                             row.entry))
            elif row.config in required and row.entry not in required[row.config]:
                findings.append("EXTRA %s %s" % (row.config, row.entry))
        elif row.config == "ctest-Release":
            if "ctest-Release" not in required:
                continue
            if row.entry in set(perf_on or ()) or row.sentinel == SENTINEL_MARK:
                continue
            if perf_on is None:
                findings.append("UNVERIFIED ctest-Release %s (pass "
                                "--perf-on-enum)" % row.entry)
            else:
                findings.append("EXTRA ctest-Release %s" % row.entry)
        elif row.entry == "default":
            continue
        elif row.entry in MANIFEST_SCOPES:
            if row.entry == "examples_manifest.txt" and row.config != "e2e-Release":
                findings.append("EXTRA %s examples_manifest.txt (only "
                                "e2e-Release consumes the examples "
                                "manifest)" % row.config)
        elif row.entry not in required.get(row.config, set()):
            findings.append("EXTRA %s %s" % (row.config, row.entry))
    for cfg in sorted(required):
        count = len(marked.get(cfg, ()))
        if not SENTINELS_PER_CONFIG[0] <= count <= SENTINELS_PER_CONFIG[1]:
            findings.append("SENTINELS %s: %d marked, need %d-%d"
                            % (cfg, count, SENTINELS_PER_CONFIG[0],
                               SENTINELS_PER_CONFIG[1]))
    return not findings, findings


def check_orphans(rows, ctest_union=None, perf_on=None, manifests=None):
    """Guard two: a renamed or dropped entry surfaces as a row whose name
    no universe contains any more. e2e names check against the two
    manifests, plain ctest rows against the union of the three decision
    trees, ctest-Release rows against the NLANG_PERF_TESTS=ON enumeration
    (the decision trees never contain them); a sentinel-marked Release row
    needs no perf enumeration because the table itself declares it.
    Returns (ok, findings)."""
    findings = []
    e2e_names = set()
    for names in (manifests or {}).values():
        e2e_names |= set(names)
    for row in rows:
        if row.entry == "default":
            continue
        if row.entry in MANIFEST_SCOPES:
            if manifests is not None and row.entry not in manifests:
                findings.append("ORPHAN %s %s (no such manifest)"
                                % (row.config, row.entry))
            continue
        if row.config in E2E_CONFIGS:
            if manifests is None:
                continue
            if row.entry not in e2e_names:
                findings.append("ORPHAN %s %s (renamed away from both e2e "
                                "manifests)" % (row.config, row.entry))
        elif row.config == "ctest-Release":
            if row.sentinel == SENTINEL_MARK:
                continue
            if perf_on is None:
                findings.append("UNVERIFIED ctest-Release %s (pass "
                                "--perf-on-enum)" % row.entry)
            elif row.entry not in perf_on:
                findings.append("ORPHAN ctest-Release %s (no such entry in "
                                "the NLANG_PERF_TESTS=ON enumeration)"
                                % row.entry)
        else:
            if ctest_union is None:
                continue
            if row.entry not in ctest_union:
                findings.append("ORPHAN %s %s (no decision tree enumerates "
                                "it any more)" % (row.config, row.entry))
    return not findings, findings


def check_constants(rows):
    """Guard seven: the constants keep the formula self-consistent
    (WATCHDOG_MULTIPLIER above SAFETY_MULTIPLIER * SCALE_BAND_MAX so the
    budget never reaches the cap at in-band scalar, MIN_WATCHDOG_S = 2 *
    MIN_CAP_S so the budget floor never lifts a budget above the floored
    cap, MIN_CAP_S above the longest sentinel reference so calibration
    sentinels never hit the floor) and every row's cap stays strictly
    above its budget at the band-edge scalar. Returns (ok, findings)."""
    findings = []
    if not WATCHDOG_MULTIPLIER > SAFETY_MULTIPLIER * SCALE_BAND_MAX:
        findings.append("CONST WATCHDOG_MULTIPLIER %s must exceed "
                        "SAFETY_MULTIPLIER %s * SCALE_BAND_MAX %s"
                        % (WATCHDOG_MULTIPLIER, SAFETY_MULTIPLIER,
                           SCALE_BAND_MAX))
    if MIN_WATCHDOG_S != 2 * MIN_CAP_S:
        findings.append("CONST MIN_WATCHDOG_S %s must equal 2 * MIN_CAP_S "
                        "%s" % (MIN_WATCHDOG_S, MIN_CAP_S))
    sent_refs = [row.ref_s for row in rows if row.sentinel == SENTINEL_MARK]
    if sent_refs and not MIN_CAP_S > max(sent_refs):
        findings.append("CONST MIN_CAP_S %s must exceed the longest "
                        "sentinel reference %.2f" % (MIN_CAP_S,
                                                     max(sent_refs)))
    for row in rows:
        budget, watchdog = caps_values(row.ref_s, 1.0, SCALE_BAND_MAX)
        if not watchdog > budget:
            findings.append("CAP %s %s: watchdog %.2f must stay above "
                            "budget %.2f" % (row.config, row.entry,
                                             watchdog, budget))
    return not findings, findings


def sentinel_rows(config, rows):
    """The sentinel-marked rows of one config in stable entry order (the
    order the calibration detail lines and candidate rows print in)."""
    return sorted((row for row in rows
                   if row.config == config and row.sentinel == SENTINEL_MARK),
                  key=lambda row: row.entry)


def sentinel_shortfall_finding(config, count):
    """Guard four admission: below SENTINELS_PER_CONFIG[0] sentinels there
    is no median worth acting on. Checked before any sampling round starts,
    so a table defect never spends a real suite run to surface."""
    if count >= SENTINELS_PER_CONFIG[0]:
        return None
    return "SENTINELS %s: %d marked, need at least %d" % (
        config, count, SENTINELS_PER_CONFIG[0])


# A junit document cannot name a killed entry (Failed and Timeout share
# status="fail", probed against CMake 3.25.2), so check_budget decides
# "watchdog kill" from the reading sitting at the cap its row computes:
# the probe measured the kill landing at cap + 1.18%, and ctest's own
# bookkeeping can sit a hair under the cap too, so the window brackets it.
# A failed entry whose span sits elsewhere in the document carries no
# workload reading (D-14: lit records failed runs as negative time for
# exactly this reason) and is only listed for the human triage.
# Cap ownership since the configure-time TIMEOUT properties landed: an
# entry with a reference row is killed at the cap its own row computes, so
# the window above reads TIMEOUT_WATCHDOG for it; a row-less entry stays
# under the legs' flat --timeout fallback, and such a kill still surfaces
# as the informational FAILED line -- a wording loss only: the fallback is
# far above every legitimate run, and guard one keeps a consumed entry
# from ever losing its row.
JUNIT_KILL_WINDOW_LO = 0.98
JUNIT_KILL_WINDOW_HI = 1.15

# D-8's self-report contract: an entry whose row prices more than the
# plain unit of reference work (the rounds axis) must print its actual
# workload on stdout -- the token is named in the entry's own unit
# (rounds=, cases=), the same unit as the row's reference-work column.
ROUNDS_TOKEN_RE = re.compile(r"rounds=(\d+)")


def _self_report_finding(name, row, report, work_factor):
    """D-8 cross-check for an entry resolved to its own row: the reading
    alone cannot tell 100 real soak rounds from 1 round on a loaded
    machine, so the workload the row prices (ref_work, the registered
    per-config rounds) must equal the rounds the entry self-reported
    (scaled by the leg's work factor). A row pricing plain work (the
    DEFAULT_WORK_FACTOR unit) has no rounds axis to cross-check; an
    entry that printed no rounds=<n> line while its row prices one is
    red, not silently default-accepted (D-8: a missing report reads as
    the coverage gap masquerading as a speedup)."""
    if row.ref_work == DEFAULT_WORK_FACTOR:
        return None
    match = ROUNDS_TOKEN_RE.search(report)
    expected = int(row.ref_work * max(float(work_factor), 1.0))
    if match is None:
        return ("SELFREPORT %s: no rounds=<n> line though the row prices "
                "ref_work=%d (the self-report contract, D-8)"
                % (name, row.ref_work))
    if int(match.group(1)) != expected:
        return ("SELFREPORT %s: rounds=%s, expected %d (ref_work=%d x "
                "work factor %g)" % (name, match.group(1), expected,
                                     row.ref_work, work_factor))
    return None


def check_budget(config, cases, rows, parallel=1, repeat="none",
                 work_factor=1.0, scalar=1.0, reports=None):
    """Guard three post-hoc for one finished ctest leg (the leg type ctest
    gives no inline knob to, F-4): judge every timed entry against the
    row it resolves to, from the leg's --output-junit timings. Caps come
    from caps_values -- the single computation place, D-5 -- with this
    same leg's calibrated scalar.

    Status handling: a run row is a completed reading; a fail row is
    either the watchdog kill -- it gets the TIMEOUT_WATCHDOG wording and
    nothing else, because a span pinned at the kill cap measured the cap
    rather than the workload (D-2's wording priority and its
    no-double-verdict clause) -- or an ordinary test failure, which
    contributes no reading and is only listed for triage (the leg's own
    verdict owns its red); notrun spans are skip-decision cost and drop
    out. When one entry carries both shapes (Task 8's --repeat), the
    fail row owns the entry and the run reading is dropped.

    Scope grading (D-3/R-17) bounds the force of a reading: only an entry
    resolved to its OWN row can make the checker red (BUDGET-FAIL); an
    entry resolved through a coarse default/manifest row gets advisory
    lines instead. A coarse scope's ratchet advisory prints once per row
    naming that scope's largest reading -- firing per entry would grind
    the coarse design back to fine every round (D-15).

    The ratchet line is reference-based and scalar-free: D-15's wording
    (reference * SAFETY_MULTIPLIER / 2, carried by the row's own work
    ratio because that half is workload, not machine) plus D-4's
    opposite-direction argument -- a contention-inflated scalar must not
    make normal readings look worth lowering the reference for. D-5 is
    not broken: the over-budget advisory and the verdict read the same
    row, the same caps() numbers and the same scalar.

    The D-8 self-report: `reports` (junit per-entry <system-out> text,
    from junit_reports) lets an entry resolved to its OWN row and priced
    above plain reference work prove its workload -- the checker
    cross-checks rounds=<n> against ref_work x work factor, and a
    missing or mismatched line is red (that is the "the rounds axis was
    set but never read" shape). The same channel dedups the inline
    verdict on the fail-triage line: a wrapper-instrumented entry the
    inline tool already judged over budget printed BUDGET-EXCEEDED and
    exited 1, so the FAILED listing names that cause instead of the
    checker adding its own budget verdict behind the leg's red (D-2's
    no-double-verdict clause).

    MISSING lines are report lines, not findings: the table is the single
    truth, a row-less entry simply cannot be judged, and the absence
    itself is guard one's territory at recording time. Returns
    (findings, detail)."""
    run_secs = {}
    fail_secs = {}
    for name, status, secs in cases:
        if status == "run":
            if name not in run_secs or secs > run_secs[name]:
                run_secs[name] = secs
        elif status == "fail":
            if name not in fail_secs or secs > fail_secs[name]:
                fail_secs[name] = secs
    measured_form = form_token(config, parallel, repeat)
    ratio = max(float(work_factor), 1.0)
    findings = []
    detail = []
    # Coarse-scope ratchet candidates bucketed per scope row:
    # scope entry -> (ratchet line, [(actual, entry), ...]).
    ratchet_buckets = {}
    for name in sorted(set(run_secs) | set(fail_secs)):
        row = resolve_scoped_row(rows, config, name)
        if row is None:
            detail.append("MISSING %s %s" % (config, name))
            continue
        if row.form != measured_form:
            findings.append("FORM %s %s recorded=%s measured=%s; re-record "
                            "the reference row in the form this leg runs "
                            "(guard ten)" % (config, name, row.form,
                                             measured_form))
            continue
        budget, watchdog = caps_values(row.ref_s, work_factor, scalar)
        ratchet = row.ref_s * ratio * SAFETY_MULTIPLIER / 2
        if name in fail_secs:
            # Fail-first merge: Task 8's --repeat can leave a passing run
            # row beside a killed (or failed) row for one entry. The fail
            # row owns the entry so the TIMEOUT_WATCHDOG wording and the
            # FAILED triage line survive next to the passing reading --
            # judging the run row first would silently drop them; the
            # run reading of the same entry is dropped instead (D-2's
            # no-double-verdict clause).
            secs = fail_secs[name]
            if (watchdog * JUNIT_KILL_WINDOW_LO <= secs
                    <= watchdog * JUNIT_KILL_WINDOW_HI):
                findings.append("BUDGET %s TIMEOUT_WATCHDOG actual=%.2f "
                                "cap=%.2f" % (name, secs, watchdog))
            else:
                inline = ""
                if "BUDGET-EXCEEDED" in (reports or {}).get(name, ""):
                    # The inline tool (the qtest wrapper) already judged
                    # this entry over budget and exited 1 -- naming the
                    # cause here keeps the triage line from reading as an
                    # unexplained test failure (D-2's no-double-verdict
                    # clause).
                    inline = " (inline BUDGET-EXCEEDED)"
                detail.append("BUDGET %s FAILED actual=%.2f%s"
                              % (name, secs, inline))
            continue
        actual = run_secs[name]
        entry_scope = row.entry == name
        if entry_scope:
            finding = _self_report_finding(
                name, row, (reports or {}).get(name, ""), work_factor)
            if finding is not None:
                findings.append(finding)
        if actual > budget:
            if entry_scope:
                findings.append("BUDGET-FAIL %s budget=%.2f actual=%.2f "
                                "(TIMEOUT_BUDGET)"
                                % (name, budget, actual))
            else:
                detail.append("advisory: %s 超预算 budget=%.2f "
                              "actual=%.2f" % (name, budget, actual))
        elif actual < ratchet:
            if entry_scope:
                detail.append("advisory: %s 建议下调参考 actual=%.2f "
                              "line=%.2f" % (name, actual, ratchet))
            else:
                ratchet_buckets.setdefault(
                    row.entry, (ratchet, []))[1].append((actual, name))
    for scope in sorted(ratchet_buckets):
        line, readings = ratchet_buckets[scope]
        actual, name = sorted(readings, key=lambda item: (-item[0],
                                                          item[1]))[0]
        detail.append("advisory: %s 建议下调参考 max_actual=%.2f "
                      "line=%.2f" % (name, actual, line))
    return findings, detail


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


def read_name_set(path):
    """Entry names from a dump file: one name per line (blank lines and
    `#` comments skipped, trailing columns such as a manifest's expected
    exit code ignored), or a ctest --show-only=json-v1 document (its
    top-level `tests` key holds the entries)."""
    text = Path(path).read_text(encoding="utf-8")
    if text.lstrip().startswith("{"):
        data = json.loads(text)
        return {t["name"] for t in data.get("tests", [])}
    names = set()
    for line in text.splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            names.add(stripped.split()[0])
    return names


def run_ctest_enum(tree):
    """Entry names of a configured tree: --show-only enumerates without
    running anything, and VS multi-config generators need -C for it."""
    proc = subprocess.run(
        ["ctest", "--test-dir", str(tree), "-C", "Debug",
         "--show-only=json-v1"],
        capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise ValueError("ctest enumeration of %s failed (rc=%d): is the "
                         "tree configured?" % (tree, proc.returncode))
    data = json.loads(proc.stdout)
    return {t["name"] for t in data.get("tests", [])}


def enum_overrides(pairs):
    overrides = {}
    for pair in pairs:
        cfg, sep, path = pair.partition("=")
        if not sep or cfg not in CONFIG_KEYS:
            raise ValueError("--enum wants CONFIG=FILE with CONFIG one of "
                             "%s, got %r" % (", ".join(CONFIG_KEYS), pair))
        overrides[cfg] = path
    return overrides
