# gate_time_guards.py -- the guard verdict bodies of the reference timing
# table: guard one (check_complete), guard two (check_orphans), guard
# seven (check_constants), the sentinel admission helpers behind guard
# four and the input readers that build the derived entry universes
# (docs/dev/gate_timing_design.md). Guard five's registered-TIMEOUT
# axis lives in the sibling gate_time_timeouts.py and guard three's
# post-hoc budget check (with guard ten's form refusal and the D-8
# self-report cross-check) in the sibling gate_time_budget.py, which
# returns (findings, detail) -- report lines, not findings. Each check
# here returns (ok, findings). The row model and the caps formula live in
# gate_time_table.py; the sampling rounds and the CLI entry live in the
# sibling gate_time_measure / gate_time_check modules. Stdlib only; run
# with -I.

import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_table import (CONFIG_KEYS, CTEST_CONFIGS, DEFAULT_TABLE,
                             E2E_CONFIGS, MANIFEST_SCOPES, MIN_CAP_S,
                             MIN_WATCHDOG_S, SAFETY_MULTIPLIER,
                             SCALE_BAND_MAX, SENTINELS_PER_CONFIG,
                             SENTINEL_MARK, WATCHDOG_MULTIPLIER,
                             caps_values)


def _enum_finding(cfg):
    """The named no-source disposition both completeness guards print
    (R-43: an unverifiable universe is a named red, never a silent
    green); one definition so the two guards' wording cannot drift."""
    return "ENUM %s: no enumeration source, rows unverifiable" % cfg


def _closing_record(cfg, table):
    """R-43: a MISSING finding names the verbatim record command that
    closes it (spec D-15: reference rows come from a real run of that
    config's own leg, so the repair has exactly one path). The ctest
    configs' numbers come from that leg's junit, the e2e configs' from
    the runner's --record-out file (spec section 9 step 0); the path
    placeholders stay literal because only the repair run knows them."""
    source = ("--from-measure <runner --record-out file>"
              if cfg in E2E_CONFIGS else
              "--from-junit <ctest --output-junit XML>")
    return "record --table %s --config %s %s" % (table, cfg, source)


def check_complete(rows, enums, manifests, perf_names=None, table=None):
    """Guard one: each derived entry needs a row in the config that
    consumes it (tree -> config key per D-2; e2e entries are covered by an
    entry row, a manifest-scope row or the config's default row), no row
    may point outside its config's derived set, and every participating
    config must carry SENTINELS_PER_CONFIG sentinel marks (D-4). The
    ctest-Release universe is the perf names (the NLANG_PERF_TESTS ON/OFF
    registration difference, R-61) union the sentinel-marked Release rows
    the table itself declares (D-3). Every MISSING finding carries the
    record command that closes it (R-43); table names the reference file
    being checked and defaults to the in-repo one. Returns (ok,
    findings)."""
    findings = []
    table = table or DEFAULT_TABLE.as_posix()
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
            findings.append(_enum_finding(cfg))
    manifest_names = set((manifests or {}).get("manifest.txt") or ())
    examples_names = set((manifests or {}).get("examples_manifest.txt") or ())
    release_marked = marked.get("ctest-Release", set())
    if perf_names is not None or release_marked:
        required["ctest-Release"] = set(perf_names or ()) | release_marked
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
            findings.append("MISSING %s %s (close: %s)"
                            % (cfg, name, _closing_record(cfg, table)))
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
            if (row.entry in set(perf_names or ())
                    or row.sentinel == SENTINEL_MARK):
                continue
            if perf_names is None:
                findings.append("UNVERIFIED ctest-Release %s (pass "
                                "--perf-names)" % row.entry)
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


def check_orphans(rows, ctest_union=None, perf_names=None, manifests=None,
                  table=None):
    """Guard two: a renamed or dropped entry surfaces as a row whose name
    no universe contains any more. e2e names check against the two
    manifests, plain ctest rows against the union of the three decision
    trees, ctest-Release rows against the perf names (the NLANG_PERF_TESTS
    ON/OFF registration difference, R-61; the decision trees never contain
    them); a sentinel-marked Release row needs no perf set because the
    table itself declares it. A missing source is named, not skipped:
    plain ctest rows without a ctest_union carry guard one's ENUM line
    per affected config, and each ORPHAN finding names its deletion
    (apply_record never removes rows, so no record command closes an
    orphan). Returns (ok, findings)."""
    findings = []
    table = table or DEFAULT_TABLE.as_posix()
    if ctest_union is None:
        # Symmetric with guard one's ENUM line (R-43's named-disposition
        # discipline at the guard layer: the Task 10 Step 5 wiring may
        # skip an unconfigured tree, but a guard fed no source must say
        # so per config instead of passing its rows as green).
        for cfg in CTEST_CONFIGS:
            if any(row.config == cfg for row in rows):
                findings.append(_enum_finding(cfg))
    e2e_names = set()
    for names in (manifests or {}).values():
        e2e_names |= set(names)
    for row in rows:
        if row.entry == "default":
            continue
        if row.entry in MANIFEST_SCOPES:
            if manifests is not None and row.entry not in manifests:
                findings.append("ORPHAN %s %s (no such manifest; delete "
                                "the row in %s)"
                                % (row.config, row.entry, table))
            continue
        if row.config in E2E_CONFIGS:
            if manifests is None:
                continue
            if row.entry not in e2e_names:
                findings.append("ORPHAN %s %s (renamed away from both e2e "
                                "manifests; delete the row in %s)"
                                % (row.config, row.entry, table))
        elif row.config == "ctest-Release":
            if row.sentinel == SENTINEL_MARK:
                continue
            if perf_names is None:
                findings.append("UNVERIFIED ctest-Release %s (pass "
                                "--perf-names)" % row.entry)
            elif row.entry not in perf_names:
                findings.append("ORPHAN ctest-Release %s (no such entry in "
                                "the NLANG_PERF_TESTS registration "
                                "difference; delete the row in %s)"
                                % (row.entry, table))
        else:
            if ctest_union is None:
                continue
            if row.entry not in ctest_union:
                findings.append("ORPHAN %s %s (no decision tree enumerates "
                                "it any more; delete the row in %s)"
                                % (row.config, row.entry, table))
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


def read_perf_names(path):
    """The perf-name roster reader shared by all four --perf-names
    consumers (spec D-17's format contract). The file holds the ON/OFF
    registration difference in one of the shapes its writers produce: one
    name per line, print-perf-names' '|'-joined value line (gate_tier.sh's
    production form), or that command's raw stdout redirect -- the value
    line plus the trailing 'GATE ' verdict line. Splitting on both
    separators and skipping empty lines, '#' comments and 'GATE ' verdict
    lines makes every spelling read as the same set."""
    text = Path(path).read_text(encoding="utf-8")
    return {name for name in re.split(r"[|\r\n]+", text)
            if name and not name.startswith("#")
            and not name.startswith("GATE ")}


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
