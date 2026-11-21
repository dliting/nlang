# gate_time_check.py -- the reference timing table, the caps()/budget
# formula and the timing guards one, two, three (post-hoc, with ten),
# four and seven (docs/dev/gate_timing_design.md). The watchdog cap and
# the budget are computed in exactly one place, caps(), per D-5. Stdlib
# only; run with -I.
#
# Subcommands (each prints one verdict line "GATE gate_time_check: PASS|FAIL"):
#   check-complete   guard one: the derived entry universe (three decision
#                    trees + the two e2e manifests, per-config sets) must be
#                    covered by reference rows, and every participating
#                    config must carry SENTINELS_PER_CONFIG sentinel marks
#   check-orphans    guard two: rows whose entry name no longer exists;
#                    ctest-Release rows are verified against the
#                    NLANG_PERF_TESTS=ON enumeration, never against the
#                    three decision trees (their universes never hold them)
#   calibrate        guard four: sample the machine scale fresh from the
#                    config's sentinel rows in the load form the timed leg
#                    will use (D-4); the scale is the median of per-sentinel
#                    measured/reference readings and must stay in
#                    [SCALE_BAND_MIN, SCALE_BAND_MAX]
#   check-budget     guard three post-hoc (ctest legs are the only leg
#                    type ctest gives no inline knob to, F-4) plus guard
#                    ten: judge a finished leg's timed entries against
#                    their rows from the --output-junit timings, with the
#                    scope grading of D-3/R-17, the ratchet of D-15 and
#                    the form-token refusal of guard ten
#   check-constants  guard seven: constant band self-consistency and every
#                    row's cap strictly above its budget at band-edge scalar
#   record           merge measured readings (--from-junit / --from-measure),
#                    refreshing ref_s and form only; the four human columns
#                    (category, ref_work, sentinel, reason) survive verbatim
#   caps             print one entry's watchdog cap and budget
#   emit-cmake       the configure-time cap emitter (D-1/D-2): one
#                    set(GATE_TIMEOUT_<entry> <watchdog cap>) line per
#                    entry with a row under any requested ctest config
#                    key, the cap the largest over those keys; the
#                    generated file is what tests/CMakeLists.txt includes,
#                    and it never mentions the machine scale (D-4)
#
# Importable consumers (D-5): caps(config, entry, work_factor=1.0,
# scalar=1.0) -> (budget_s, watchdog_s) and load_table(path).

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from collections import namedtuple
from pathlib import Path

# Frozen values pinned by the design (D-1/D-3/D-7); changing one changes
# the caps() contract every consumer shares.
SAFETY_MULTIPLIER = 2
WATCHDOG_MULTIPLIER = 10
MIN_CAP_S = 60
MIN_WATCHDOG_S = 120
SCALE_BAND_MIN = 0.25
SCALE_BAND_MAX = 4.0
DEFAULT_WORK_FACTOR = 1
SLOW_MIN_REF_S = 300

SENTINEL_MARK = "yes"
SENTINELS_PER_CONFIG = (5, 8)
CTEST_CONFIGS = ("ctest-Debug", "ctest-IDE", "ctest-ASan")
E2E_CONFIGS = ("e2e-Debug", "e2e-ASan", "e2e-Release")
CONFIG_KEYS = CTEST_CONFIGS + ("ctest-Release",) + E2E_CONFIGS
CATEGORIES = ("常规", "慢功能", "演示验收", "性能")
MANIFEST_SCOPES = ("manifest.txt", "examples_manifest.txt")
LOCAL_TABLE_NAME = "gate_time_reference.local.txt"
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TABLE = REPO_ROOT / "tests" / "gate_time_reference.txt"
# Each decision tree feeds exactly one ctest config key (D-2).
DECISION_TREES = {"ctest-Debug": "build",
                  "ctest-IDE": "build-ide",
                  "ctest-ASan": "build-asan"}

Row = namedtuple("Row", "config entry ref_s ref_work category form sentinel "
                        "reason line")


def load_table(path):
    """Parse one reference table file. One row per (config, entry),
    whitespace-separated: config entry ref_s ref_work category form
    [sentinel [reason]]; `reason` runs to end of line, so a row with seven
    tokens carries no sentinel and a row with six neither. Blank lines and
    `#` comments are skipped. Raises ValueError on a malformed row."""
    rows = []
    text = Path(path).read_text(encoding="utf-8")
    for lineno, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split(None, 7)
        if len(fields) < 6:
            raise ValueError("%s line %d: expected at least six columns, "
                             "got %d" % (path, lineno, len(fields)))
        config, entry, ref_s, ref_work, category, form = fields[:6]
        sentinel = fields[6] if len(fields) > 6 else ""
        reason = fields[7] if len(fields) > 7 else ""
        if config not in CONFIG_KEYS:
            raise ValueError("%s line %d: unknown config key %r (want one "
                             "of %s)" % (path, lineno, config,
                                         ", ".join(CONFIG_KEYS)))
        if category not in CATEGORIES:
            raise ValueError("%s line %d: unknown category %r (want one of "
                             "%s)" % (path, lineno, category,
                                      ", ".join(CATEGORIES)))
        try:
            ref_s = float(ref_s)
            ref_work = int(ref_work)
        except ValueError:
            raise ValueError("%s line %d: ref_s/ref_work must be numeric"
                             % (path, lineno))
        if ref_s < 0.0 or ref_work < 1:
            raise ValueError("%s line %d: ref_s must be >= 0 and ref_work "
                             ">= 1" % (path, lineno))
        rows.append(Row(config, entry, ref_s, ref_work, category, form,
                        sentinel, reason, lineno))
    return rows


def _effective_rows(table_path):
    """Rows of the checked-in table with the machine-local overlay applied
    (D-3): gate_time_reference.local.txt beside it overrides same
    (config, entry) rows and may add rows; it is never checked in."""
    rows = load_table(table_path)
    local = Path(table_path).resolve().parent / LOCAL_TABLE_NAME
    if not local.is_file():
        return rows
    overlay = {(r.config, r.entry): r for r in load_table(local)}
    merged = [overlay.pop((r.config, r.entry), r) for r in rows]
    merged.extend(overlay.values())
    return merged


def _caps_values(ref_s, work_factor, scalar):
    """D-1 verbatim: work ratio = max(reference work scaled, 1); watchdog
    cap = max(ref_s * ratio * WATCHDOG_MULTIPLIER, MIN_WATCHDOG_S); budget
    = clamp(ref_s * ratio * scalar * SAFETY_MULTIPLIER, MIN_CAP_S, cap).
    The scalar scales the budget only -- the watchdog stays machine-free
    so a slow machine never loses its hang detection."""
    ratio = max(float(work_factor), 1.0)
    watchdog = max(ref_s * ratio * WATCHDOG_MULTIPLIER, float(MIN_WATCHDOG_S))
    budget = min(max(ref_s * ratio * scalar * SAFETY_MULTIPLIER,
                     float(MIN_CAP_S)), watchdog)
    return budget, watchdog


def _resolve_row(rows, config, entry):
    """Entry scope first, then the `default` row of an e2e config (D-3);
    ctest configs list every entry per row and have no fallback."""
    for row in rows:
        if row.config == config and row.entry == entry:
            return row
    if config in E2E_CONFIGS:
        for row in rows:
            if row.config == config and row.entry == "default":
                return row
    raise KeyError("%s %s" % (config, entry))


def _resolve_scoped_row(rows, config, entry):
    """Row resolution for the post-hoc checker, widened to the coarse
    scopes (D-3/R-17) in D-3's fixed priority: entry row > manifest row >
    `default` row (nearest scope wins). ctest configs list every entry and
    carry no coarse row today, so an unknown name resolves to None there
    and the checker reports it missing rather than inventing a budget.
    The grading of what a reading can do (red versus advisory) lives in
    check_budget, not here."""
    for row in rows:
        if row.config == config and row.entry == entry:
            return row
    for scope in MANIFEST_SCOPES + ("default",):
        for row in rows:
            if row.config == config and row.entry == scope:
                return row
    return None


def caps(config, entry, work_factor=1.0, scalar=1.0, table_path=None):
    """(budget_s, watchdog_s) for one entry: the single place caps and
    budgets are computed (D-5)."""
    path = DEFAULT_TABLE if table_path is None else table_path
    row = _resolve_row(_effective_rows(path), config, entry)
    return _caps_values(row.ref_s, work_factor, scalar)


def emit_cmake(configs, out_path, work_factor=DEFAULT_WORK_FACTOR,
               table_path=DEFAULT_TABLE):
    """Write the configure-time TIMEOUT cap file (D-1/D-2): one
    `set(GATE_TIMEOUT_<sanitized entry> <watchdog cap>)` line per entry
    with a row under any requested config key, the cap the largest
    watchdog cap over those keys (one TIMEOUT property cannot switch with
    ctest -C, so the widest of the tree's keys wins). Names sanitize every
    character outside [A-Za-z0-9_] to '_', the same rule the add_test
    wrapper applies when it looks the variable up. The machine scale never
    participates: it reaches test processes only through the leg
    environment (D-4), so the file stays a pure function of the table plus
    the work factor. A work factor below 1 is refused (D-8: the
    configure-time knob may widen caps above the committed table but never
    shrink them). An entry with no row under any requested key gets no
    variable at all: the leg's ctest --timeout fallback owns it. Returns
    the number of entries emitted."""
    try:
        factor = float(work_factor)
    except (TypeError, ValueError):
        raise ValueError("work factor %r is not a number" % (work_factor,))
    if factor < 1.0:
        raise ValueError(
            "work factor %r is below 1.0: the configure-time knob may "
            "widen the caps above the committed table but never shrink "
            "them (D-8)" % (work_factor,))
    keys = []
    for config in configs:
        if config not in CONFIG_KEYS:
            raise ValueError("unknown config key %r (legal keys: %s)"
                             % (config, ", ".join(CONFIG_KEYS)))
        if config not in keys:
            keys.append(config)
    if not keys:
        raise ValueError("emit-cmake needs at least one --config key")
    watchdogs = {}
    for row in _effective_rows(Path(table_path)):
        if row.config not in keys or row.entry == "default":
            continue
        # caps() per row (never an inlined product): one formula site,
        # D-5. A few hundred rows re-resolving the table is milliseconds
        # at configure time.
        _budget, watchdog = caps(row.config, row.entry, work_factor=factor,
                                 table_path=table_path)
        prev = watchdogs.get(row.entry)
        if prev is None or watchdog > prev:
            watchdogs[row.entry] = watchdog
    lines = [
        "# Generated by tools/gate_time_check.py emit-cmake: the ctest",
        "# TIMEOUT watchdog caps derived from the committed reference",
        "# table (gate timing design D-1/D-2). Edit the table, not this",
        "# file; the configure step regenerates it whenever the table",
        "# changes.",
        "# configs: %s" % " ".join(keys),
        "# work_factor: %r" % factor,
    ]
    for entry in sorted(watchdogs):
        var = "GATE_TIMEOUT_" + re.sub(r"[^A-Za-z0-9_]", "_", entry)
        lines.append("set(%s %r)" % (var, watchdogs[entry]))
    # Explicit newline="\n" (same reasoning as apply_record): the
    # generated file stays LF on every platform.
    with Path(out_path).open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    return len(watchdogs)


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
        budget, watchdog = _caps_values(row.ref_s, 1.0, SCALE_BAND_MAX)
        if not watchdog > budget:
            findings.append("CAP %s %s: watchdog %.2f must stay above "
                            "budget %.2f" % (row.config, row.entry,
                                             watchdog, budget))
    return not findings, findings


def _sentinel_rows(config, rows):
    """The sentinel-marked rows of one config in stable entry order (the
    order the calibration detail lines and candidate rows print in)."""
    return sorted((row for row in rows
                   if row.config == config and row.sentinel == SENTINEL_MARK),
                  key=lambda row: row.entry)


def _sentinel_shortfall_finding(config, count):
    """Guard four admission: below SENTINELS_PER_CONFIG[0] sentinels there
    is no median worth acting on. Checked before any sampling round starts,
    so a table defect never spends a real suite run to surface."""
    if count >= SENTINELS_PER_CONFIG[0]:
        return None
    return "SENTINELS %s: %d marked, need at least %d" % (
        config, count, SENTINELS_PER_CONFIG[0])


def calibrate_scale(config, measurements, rows):
    """Guard four (D-4): the machine scale is the median of the per-sentinel
    measured/reference ratios -- the median over the mean so one
    pathological reading cannot drag every budget of the machine. Returns
    (scale, findings, candidate_rows); scale is None when no ratio could be
    computed. A scale outside [SCALE_BAND_MIN, SCALE_BAND_MAX] is not a
    slow machine but a table-versus-machine disagreement: the finding names
    the config whose reference needs re-recording, and the candidate rows
    carry this round's readings in table shape for that re-record. The
    band and the MIN_CAP_S-above-sentinel-references clause of guard seven
    together keep a calibration reading inside its budget floor."""
    sent = _sentinel_rows(config, rows)
    shortfall = _sentinel_shortfall_finding(config, len(sent))
    findings = [] if shortfall is None else [shortfall]
    if findings:
        return None, findings, []
    ratios = []
    candidates = []
    for row in sent:
        if row.ref_s <= 0.0:
            findings.append("CALIB %s %s: sentinel ref_s must be positive "
                            "to form a ratio" % (config, row.entry))
            continue
        reading = measurements.get(row.entry)
        if reading is None:
            findings.append("CALIB %s %s: no reading from the sample run"
                            % (config, row.entry))
            continue
        secs = reading[0]
        ratios.append(secs / row.ref_s)
        candidates.append(_row_line(config, row.entry, secs, row.ref_work,
                                    row.category, row.form, row.sentinel,
                                    row.reason))
    if not ratios:
        return None, findings, candidates
    scale = statistics.median(ratios)
    if not SCALE_BAND_MIN <= scale <= SCALE_BAND_MAX:
        findings.append("BAND %s: scale %.3f outside [%s, %s]; re-record "
                        "the reference rows of %s from the CANDIDATE rows "
                        "and re-run (a machine this far from its recorded "
                        "table is not calibratable)"
                        % (config, scale, SCALE_BAND_MIN, SCALE_BAND_MAX,
                           config))
    return scale, findings, candidates


def parse_measure_rows(text):
    """Parse Task 1 MEASURE rows (`MEASURE <name> <seconds> form=<form>`).
    The directory-form compile-exception path can emit two rows for one
    entry (pre-existing double verdict): merge per name keeping the
    maximum reading, never sum, and a lone row is not an error. Other
    lines are ignored."""
    measurements = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("MEASURE "):
            continue
        fields = stripped.split()
        if len(fields) < 3:
            raise ValueError("malformed MEASURE row: %r" % line)
        name = fields[1]
        try:
            secs = float(fields[2])
        except ValueError:
            raise ValueError("malformed MEASURE row: %r" % line)
        form = ""
        for token in fields[3:]:
            if token.startswith("form="):
                form = token[5:]
        prev = measurements.get(name)
        if prev is None or secs > prev[0]:
            measurements[name] = (secs, form or (prev[1] if prev else ""))
    return measurements


def _junit_cases(xml_text):
    """The raw view of a ctest --output-junit document: (name, status,
    seconds) triples for every <testcase> carrying all three attributes
    (a row missing one carries nothing trustworthy; the record path has
    always refused those, and the budget checker skips them the same
    way). The CMake 3.25.2 status semantics -- Passed="run", Failed and
    Timeout both="fail" with no attribute telling them apart,
    skipped="notrun" -- are documented at each consumer's filter."""
    cases = []
    for case in ET.fromstring(xml_text).iter("testcase"):
        name = case.get("name")
        status = case.get("status")
        raw = case.get("time")
        if not name or status is None or raw is None:
            continue
        cases.append((name, status, float(raw)))
    return cases


def parse_junit_rows(xml_text, form="ctest-junit-serial"):
    """Parse ctest --output-junit XML into {name: (seconds, form)}, taking
    only <testcase> elements with status="run" (R-7: ref_s means seconds
    for one completed reference workload). Shapes verified against CMake
    3.25.2: Passed carries status="run" with the real duration; Failed
    carries status="fail" with the full span; Timeout carries the same
    status="fail" (no attribute distinguishes it) with a time at the
    then-active cap; a skipped test (SKIP_RETURN_CODE or
    SKIP_REGULAR_EXPRESSION) carries status="notrun" with a time that is
    only the skip-decision cost; entries filtered out by -R/-E emit no
    <testcase> at all. So fail and notrun rows stay out of the readings,
    and a row without the status attribute is refused too (no reading
    beats a wrong reading); excluded entries keep their placeholder or
    previous value and surface through the leg's gate.sh verdict and the
    remaining-0.00-rows listing. Same-name run duplicates keep the
    maximum (never sum)."""
    measurements = {}
    for name, status, secs in _junit_cases(xml_text):
        if status != "run":
            continue
        if name not in measurements or secs > measurements[name][0]:
            measurements[name] = (secs, form)
    return measurements


def _row_line(config, entry, ref_s, ref_work, category, form, sentinel,
              reason):
    # A row never carries a reason without a sentinel token in front of
    # it (the parser would have read the reason as the sentinel), so the
    # sentinel token gates the reason token.
    line = "%s %s %.2f %s %s %s" % (config, entry, ref_s, ref_work,
                                    category, form)
    if sentinel or reason:
        line += " %s" % (sentinel or "-")
    if reason:
        line += " %s" % reason
    return line


def apply_record(table_path, config, measurements):
    """Merge measured readings into the table. Entry rows of `config`
    refresh ref_s and form only; category, ref_work, sentinel and reason
    keep their original tokens verbatim (guard six member), and every
    other line (comments included) is rewritten untouched. A config
    without a default row gains rows for measured entries it had none
    for; an e2e config with a default row already covers new names.
    Returns (updated, added)."""
    if config not in CONFIG_KEYS:
        raise ValueError("unknown config key: %r" % config)
    path = Path(table_path)
    lines = path.read_text(encoding="utf-8").splitlines()
    rows = load_table(path)
    has_default = any(r.config == config and r.entry == "default"
                      for r in rows)
    measured = {r.entry for r in rows
                if r.config == config and r.entry in measurements
                and r.entry != "default" and r.entry not in MANIFEST_SCOPES}
    out = []
    updated = 0
    for line in lines:
        stripped = line.strip()
        fields = stripped.split(None, 7)
        if (stripped and not stripped.startswith("#") and len(fields) >= 6
                and fields[0] == config and fields[1] in measured):
            secs, form = measurements[fields[1]]
            sentinel = fields[6] if len(fields) > 6 else ""
            reason = fields[7] if len(fields) > 7 else ""
            out.append(_row_line(config, fields[1], secs, fields[3],
                                 fields[4], form or fields[5], sentinel,
                                 reason))
            updated += 1
        else:
            out.append(line)
    added = []
    if not has_default:
        for name in sorted(set(measurements) - measured):
            secs, form = measurements[name]
            out.append(_row_line(config, name, secs,
                                 str(DEFAULT_WORK_FACTOR), "常规",
                                 form or "unknown", "", ""))
            added.append(name)
    # Explicit newline="\n": the table stays LF on every platform (the
    # write_text(newline=) form needs Python 3.10, the trees' cached
    # Python3 is older).
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(out) + "\n")
    return updated, added


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


def _form_token(config, parallel, repeat):
    """Guard ten's measurement-form token for a reading taken now: the
    family half says which product timed it (ctest junit versus the e2e
    runner's MEASURE rows), the mode half how the leg was driven. A
    reading may only be judged against a row recorded in the same form:
    a serial reference compared against parallel contention prices the
    contention into the machine scalar and misjudges both (D-2's last
    paragraph)."""
    family = "ctest-junit" if config.startswith("ctest-") else "e2e"
    mode = "-serial" if parallel <= 1 else "-parallel-%d" % parallel
    if repeat and repeat != "none":
        mode += "-repeat-%s" % repeat
    return family + mode


def check_budget(config, cases, rows, parallel=1, repeat="none",
                 work_factor=1.0, scalar=1.0):
    """Guard three post-hoc for one finished ctest leg (the leg type ctest
    gives no inline knob to, F-4): judge every timed entry against the
    row it resolves to, from the leg's --output-junit timings. Caps come
    from _caps_values -- the single computation place, D-5 -- with this
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
    measured_form = _form_token(config, parallel, repeat)
    ratio = max(float(work_factor), 1.0)
    findings = []
    detail = []
    # Coarse-scope ratchet candidates bucketed per scope row:
    # scope entry -> (ratchet line, [(actual, entry), ...]).
    ratchet_buckets = {}
    for name in sorted(set(run_secs) | set(fail_secs)):
        row = _resolve_scoped_row(rows, config, name)
        if row is None:
            detail.append("MISSING %s %s" % (config, name))
            continue
        if row.form != measured_form:
            findings.append("FORM %s %s recorded=%s measured=%s; re-record "
                            "the reference row in the form this leg runs "
                            "(guard ten)" % (config, name, row.form,
                                             measured_form))
            continue
        budget, watchdog = _caps_values(row.ref_s, work_factor, scalar)
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
                detail.append("BUDGET %s FAILED actual=%.2f" % (name, secs))
            continue
        actual = run_secs[name]
        entry_scope = row.entry == name
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


def _read_name_set(path):
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


def _run_ctest_enum(tree):
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


def _sample_ctest(config, tree, names):
    """Guard four sampling round for a ctest config (D-4): the sentinel set
    runs once through a fresh ctest -R, in exactly the form the timed legs
    use it (serial, -C Debug; ctest-Release reads the perf-on tree whose
    config runs Release), and the readings come back through
    --output-junit. A sentinel that fails or is killed during sampling
    carries no run row, and the missing-reading finding reports it; the
    round's own rc is not the verdict (a killed sentinel is what the band
    decision and the findings are for)."""
    cfg = "Release" if config == "ctest-Release" else "Debug"
    # re.escape each name before the union: ctest -R is a regex, so one
    # entry name carrying a metacharacter must not widen (or break) the
    # sentinel set -- the escape holds regardless of name discipline.
    regex = "^(" + "|".join(re.escape(name) for name in sorted(names)) + ")$"
    with tempfile.TemporaryDirectory() as tmp:
        xml_path = Path(tmp) / "calibrate.xml"
        proc = subprocess.run(
            ["ctest", "--test-dir", str(tree), "-C", cfg, "-R", regex,
             "--output-junit", str(xml_path)],
            capture_output=True, text=True, encoding="utf-8")
        if not xml_path.is_file():
            raise ValueError("calibration ctest run in %s produced no junit "
                             "(rc=%d): is the tree configured?"
                             % (tree, proc.returncode))
        return parse_junit_rows(xml_path.read_text(encoding="utf-8"))


def _sample_e2e(config, tools_tree, names, manifest_path):
    """Guard four sampling round for an e2e config (D-4): the manifest
    lines whose first column is a sentinel form a derived subset (the
    runner takes one manifest per run, and subsets are derived artifacts,
    never stored), run through the runner with --record-out in the legs'
    serial form. --timeout=120 matches the 120 s legs; every sentinel
    reference sits well below it (guard seven's MIN_CAP_S clause), so the
    flag cannot shape a calibration reading."""
    cfg = "Release" if config == "e2e-Release" else "Debug"
    kept = [line for line in
            Path(manifest_path).read_text(encoding="utf-8").splitlines()
            if line.strip() and line.strip().split()[0] in names]
    runner = REPO_ROOT / "tests" / "e2e" / "run_e2e_tests.py"
    tools = Path(tools_tree) / "src" / "tools"
    with tempfile.TemporaryDirectory() as tmp:
        subset = Path(tmp) / "sentinels.txt"
        record = Path(tmp) / "record.txt"
        with subset.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(kept) + "\n")
        env = dict(os.environ, NLANG_E2E_MANIFEST=str(subset))
        proc = subprocess.run(
            [sys.executable, str(runner),
             str(tools / "ncc" / cfg / "ncc.exe"),
             str(tools / "nvm" / cfg / "nvm.exe"),
             str(tools / "ndb" / cfg / "ndb.exe"),
             "--timeout=120", "--record-out", str(record)],
            cwd=str(runner.parent), env=env, capture_output=True,
            text=True, encoding="utf-8")
        if not record.is_file():
            raise ValueError("calibration e2e run against %s produced no "
                             "record file (rc=%d): are the tools built?"
                             % (tools_tree, proc.returncode))
        return parse_measure_rows(record.read_text(encoding="utf-8"))


def _enum_overrides(pairs):
    overrides = {}
    for pair in pairs:
        cfg, sep, path = pair.partition("=")
        if not sep or cfg not in CONFIG_KEYS:
            raise ValueError("--enum wants CONFIG=FILE with CONFIG one of "
                             "%s, got %r" % (", ".join(CONFIG_KEYS), pair))
        overrides[cfg] = path
    return overrides


def _inputs(args, repo):
    """Derived universes for guards one/two: live --show-only enumerations
    per decision tree unless --enum pins a config to a dump file;
    --perf-on-enum (or --enum ctest-Release=FILE) feeds the ctest-Release
    universe; both e2e manifests default to the repo lists."""
    overrides = _enum_overrides(args.enum)
    enums = {}
    for cfg, tree in DECISION_TREES.items():
        if cfg in overrides:
            enums[cfg] = _read_name_set(overrides[cfg])
        else:
            enums[cfg] = _run_ctest_enum(repo / tree)
    perf_on = None
    if args.perf_on_enum:
        perf_on = _read_name_set(args.perf_on_enum)
    elif "ctest-Release" in overrides:
        perf_on = _read_name_set(overrides["ctest-Release"])
    manifests = {
        "manifest.txt": _read_name_set(
            args.manifest or repo / "tests" / "e2e" / "manifest.txt"),
        "examples_manifest.txt": _read_name_set(
            args.examples_manifest
            or repo / "tests" / "e2e" / "examples_manifest.txt"),
    }
    return enums, perf_on, manifests


def _verdict(findings):
    for finding in findings:
        print(finding)
    if findings:
        print("GATE gate_time_check: FAIL (%d findings)" % len(findings))
        return 1
    print("GATE gate_time_check: PASS")
    return 0


def _dispatch(args):
    table = Path(args.table) if args.table else DEFAULT_TABLE
    if args.command == "caps":
        if args.config not in CONFIG_KEYS:
            # Name the misspelling against the legal vocabulary: a
            # mistyped config must not read as "add a default row" (the
            # KeyError wording below describes missing rows only).
            return _verdict(["CAPS %s %s: unknown config key (want one of "
                             "%s)" % (args.config, args.entry,
                                      ", ".join(CONFIG_KEYS))])
        try:
            budget, watchdog = caps(args.config, args.entry,
                                    work_factor=args.work_factor,
                                    scalar=args.scalar, table_path=table)
        except KeyError:
            print("CAPS %s %s: no reference row and no default row for "
                  "this config" % (args.config, args.entry))
            return _verdict(["CAPS %s %s: unresolved" % (args.config,
                                                         args.entry)])
        print("CAPS %s %s budget_s=%.2f watchdog_s=%.2f work_factor=%g "
              "scalar=%g" % (args.config, args.entry, budget, watchdog,
                             args.work_factor, args.scalar))
        return _verdict([])
    if args.command == "emit-cmake":
        # A bad work factor or config key raises before anything is
        # written: main's handler turns it into the ERROR + FAIL(error)
        # pair the CMake FATAL_ERROR quotes back (D-8), and the stale
        # generated file from the previous configure stays untouched.
        count = emit_cmake(args.configs, args.out,
                           work_factor=args.work_factor, table_path=table)
        print("EMIT-CMAKE entries=%d out=%s" % (count, args.out))
        return _verdict([])
    if args.command == "record":
        if bool(args.from_junit) == bool(args.from_measure):
            raise ValueError("record needs exactly one of --from-junit / "
                             "--from-measure")
        text = Path(args.from_junit or args.from_measure).read_text(
            encoding="utf-8")
        if args.from_junit:
            measurements = parse_junit_rows(text,
                                            form=args.form or "ctest-junit-serial")
        else:
            measurements = parse_measure_rows(text)
            if args.form:
                measurements = {name: (secs, args.form)
                                for name, (secs, _) in measurements.items()}
        updated, added = apply_record(table, args.config, measurements)
        print("RECORD %s updated=%d added=%d" % (args.config, updated,
                                                 len(added)))
        return _verdict([])
    if args.command == "calibrate":
        if args.config not in CONFIG_KEYS:
            return _verdict(["CALIBRATE %s: unknown config key (want one "
                             "of %s)" % (args.config,
                                         ", ".join(CONFIG_KEYS))])
        is_ctest = (args.config in CTEST_CONFIGS
                    or args.config == "ctest-Release")
        if is_ctest and (not args.tree or args.tools):
            raise ValueError("calibrate %s wants --tree (a configured "
                             "decision tree)" % args.config)
        if not is_ctest and (not args.tools or args.tree):
            raise ValueError("calibrate %s wants --tools (the tree holding "
                             "the ncc/nvm/ndb binaries)" % args.config)
        rows = _effective_rows(table)
        sent = _sentinel_rows(args.config, rows)
        shortfall = _sentinel_shortfall_finding(args.config, len(sent))
        if shortfall:
            return _verdict([shortfall])
        names = {row.entry for row in sent}
        if is_ctest:
            measurements = _sample_ctest(args.config, args.tree, names)
        else:
            manifest = (args.manifest
                        or str(REPO_ROOT / "tests" / "e2e" / "manifest.txt"))
            measurements = _sample_e2e(args.config, args.tools, names,
                                       manifest)
        for row in sent:
            reading = measurements.get(row.entry)
            if reading is not None and row.ref_s > 0.0:
                print("CALIB %s %s measured=%.2f ref=%.2f ratio=%.3f"
                      % (args.config, row.entry, reading[0], row.ref_s,
                         reading[0] / row.ref_s))
        scale, findings, candidates = calibrate_scale(args.config,
                                                      measurements, rows)
        if scale is not None:
            # %s on the band edges keeps the constants' own canonical
            # spelling (0.25 and 4.0) rather than a format that would drop
            # the trailing zero (%g) or pad it (%.2f).
            print("SCALE %s scale=%.3f band=[%s,%s]"
                  % (args.config, scale, SCALE_BAND_MIN, SCALE_BAND_MAX))
        # The candidate rows are the re-record input, printed exactly when
        # the round is red (the band finding, or readings the table cannot
        # yet explain): they are the CANDIDATE wording of guard four.
        if findings:
            findings += ["CANDIDATE " + line for line in candidates]
        return _verdict(findings)
    if args.command == "check-budget":
        if args.config not in CONFIG_KEYS:
            return _verdict(["CHECK-BUDGET %s: unknown config key (want "
                             "one of %s)" % (args.config,
                                             ", ".join(CONFIG_KEYS))])
        cases = _junit_cases(Path(args.junit).read_text(encoding="utf-8"))
        findings, detail = check_budget(
            args.config, cases, _effective_rows(table),
            parallel=args.parallel, repeat=args.repeat,
            work_factor=args.work_factor, scalar=args.scalar)
        # Detail lines (advisories, MISSING, triage listings) print in
        # both outcomes: they carry the context the verdict and the
        # findings alone would leave out.
        for line in detail:
            print(line)
        return _verdict(findings)
    rows = _effective_rows(table)
    if args.command == "check-constants":
        return _verdict(check_constants(rows)[1])
    enums, perf_on, manifests = _inputs(args, Path(args.repo))
    for cfg in sorted(enums):
        print("ENUM %s: %d entries" % (cfg, len(enums[cfg])))
    if perf_on is not None:
        print("ENUM ctest-Release: %d entries (NLANG_PERF_TESTS=ON form)"
              % len(perf_on))
    if args.command == "check-complete":
        return _verdict(check_complete(rows, enums, manifests,
                                       perf_on)[1])
    ctest_union = set().union(*enums.values())
    return _verdict(check_orphans(rows, ctest_union, perf_on,
                                  manifests)[1])


def _build_parser():
    parser = argparse.ArgumentParser(
        description="Reference timing table guards (see the module "
                    "docstring).")
    sub = parser.add_subparsers(dest="command", required=True)
    # One shared definition of --table: every subcommand takes the flag
    # with the same name, default and help text, and the parents
    # mechanism carries the same action into each parser, so the flag
    # can never drift between subcommands.
    table_arg = argparse.ArgumentParser(add_help=False)
    table_arg.add_argument("--table", help="reference table (default "
                                           "tests/gate_time_reference.txt)")
    derived = argparse.ArgumentParser(add_help=False, parents=[table_arg])
    derived.add_argument("--repo", default=str(REPO_ROOT),
                         help="repo root holding the decision trees "
                             "(default the tool's own repo)")
    derived.add_argument("--enum", action="append", default=[],
                         metavar="CONFIG=FILE",
                         help="pin one config's universe to a name dump or "
                              "ctest json (repeatable)")
    derived.add_argument("--perf-on-enum",
                         help="ctest-Release universe: entry names "
                              "enumerated with NLANG_PERF_TESTS=ON")
    derived.add_argument("--manifest", help="e2e manifest list file")
    derived.add_argument("--examples-manifest",
                         help="e2e examples manifest list file")
    sub.add_parser("check-complete", parents=[derived],
                   help="guard one: coverage and sentinel counts")
    sub.add_parser("check-orphans", parents=[derived],
                   help="guard two: rows for entries that no longer exist")
    sub.add_parser("check-constants", parents=[table_arg],
                   help="guard seven: constant band self-consistency")
    record = sub.add_parser("record", parents=[table_arg],
                            help="merge measured readings into the table")
    record.add_argument("--config", required=True,
                        help="config key the readings belong to")
    record.add_argument("--from-junit", metavar="FILE",
                        help="ctest --output-junit XML")
    record.add_argument("--from-measure", metavar="FILE",
                        help="runner --record-out MEASURE file")
    record.add_argument("--form", help="measurement form (default per "
                                       "source)")
    caps_p = sub.add_parser("caps", parents=[table_arg],
                            help="print one entry's cap and budget")
    caps_p.add_argument("config")
    caps_p.add_argument("entry")
    caps_p.add_argument("--work-factor", type=float, default=1.0)
    caps_p.add_argument("--scalar", type=float, default=1.0)
    emit_p = sub.add_parser("emit-cmake", parents=[table_arg],
                            help="write the configure-time ctest TIMEOUT "
                                 "caps file from the table")
    emit_p.add_argument("--config", dest="configs", action="append",
                        help="config key whose rows feed the caps "
                             "(repeatable; exactly the keys of the tree "
                             "being configured)")
    emit_p.add_argument("--work-factor", type=float,
                        default=float(DEFAULT_WORK_FACTOR),
                        help="configure-time work scaling, D-8 (>= 1; "
                             "omit entirely when the environment gives no "
                             "factor)")
    emit_p.add_argument("--out", required=True, metavar="FILE",
                        help="generated CMake file to write")
    calibrate_p = sub.add_parser("calibrate", parents=[table_arg],
                                 help="guard four: sample the machine "
                                      "scale from a config's sentinels")
    calibrate_p.add_argument("config", help="config key to calibrate")
    calibrate_p.add_argument("--tree", help="configured decision tree to "
                                            "sample (ctest configs)")
    calibrate_p.add_argument("--tools", help="tree holding the ncc/nvm/ndb "
                                             "binaries to sample (e2e "
                                             "configs)")
    calibrate_p.add_argument("--manifest", help="e2e manifest the sentinel "
                                                "subset derives from "
                                                "(default tests/e2e/"
                                                "manifest.txt)")
    budget_p = sub.add_parser("check-budget", parents=[table_arg],
                              help="guard three post-hoc: judge a finished "
                                   "ctest leg's budgets from its junit "
                                   "timings")
    budget_p.add_argument("--junit", required=True, metavar="FILE",
                          help="the leg's ctest --output-junit XML")
    budget_p.add_argument("--config", required=True,
                          help="config key the leg ran under")
    budget_p.add_argument("--parallel", type=int, default=1,
                          help="the -j level the leg ran at (guard ten's "
                               "form token)")
    budget_p.add_argument("--repeat", default="none",
                          help="the --repeat mode the leg ran at (guard "
                               "ten's form token)")
    budget_p.add_argument("--work-factor", type=float, default=1.0,
                          help="work scaling the leg ran at (the D-1 "
                               "ratio)")
    budget_p.add_argument("--scalar", type=float, default=1.0,
                          help="the machine scale calibration measured for "
                               "this very leg (D-5: same leg, same number)")
    return parser


def main(argv=None):
    args = _build_parser().parse_args(argv)
    try:
        return _dispatch(args)
    except (ValueError, OSError, ET.ParseError) as exc:
        print("ERROR %s" % exc)
        print("GATE gate_time_check: FAIL (error)")
        return 1


if __name__ == "__main__":
    sys.exit(main())
