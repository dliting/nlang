# gate_time_check.py -- the reference timing table, the caps()/budget
# formula and the timing guards one, two and seven
# (docs/dev/gate_timing_design.md). The watchdog cap and the budget are
# computed in exactly one place, caps(), per D-5. Stdlib only; run with -I.
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
#   check-constants  guard seven: constant band self-consistency and every
#                    row's cap strictly above its budget at band-edge scalar
#   record           merge measured readings (--from-junit / --from-measure),
#                    refreshing ref_s and form only; the four human columns
#                    (category, ref_work, sentinel, reason) survive verbatim
#   caps             print one entry's watchdog cap and budget
#
# Importable consumers (D-5): caps(config, entry, work_factor=1.0,
# scalar=1.0) -> (budget_s, watchdog_s) and load_table(path).

import argparse
import json
import subprocess
import sys
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


def caps(config, entry, work_factor=1.0, scalar=1.0, table_path=None):
    """(budget_s, watchdog_s) for one entry: the single place caps and
    budgets are computed (D-5)."""
    path = DEFAULT_TABLE if table_path is None else table_path
    row = _resolve_row(_effective_rows(path), config, entry)
    return _caps_values(row.ref_s, work_factor, scalar)


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


def parse_junit_rows(xml_text, form="ctest-junit-serial"):
    """Parse ctest --output-junit XML into {name: (seconds, form)} from
    each <testcase name time>. Same-name duplicates keep the maximum
    (never sum); testcases without a time attribute (skips) are ignored."""
    measurements = {}
    for case in ET.fromstring(xml_text).iter("testcase"):
        name = case.get("name")
        raw = case.get("time")
        if not name or raw is None:
            continue
        secs = float(raw)
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
    derived = argparse.ArgumentParser(add_help=False)
    derived.add_argument("--table", help="reference table (default "
                                         "tests/gate_time_reference.txt)")
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
    constants = argparse.ArgumentParser(add_help=False)
    constants.add_argument("--table", help="reference table (default "
                                           "tests/gate_time_reference.txt)")
    sub.add_parser("check-constants", parents=[constants],
                   help="guard seven: constant band self-consistency")
    record = sub.add_parser("record", help="merge measured readings into "
                                           "the table")
    record.add_argument("--table", help="reference table (default "
                                        "tests/gate_time_reference.txt)")
    record.add_argument("--config", required=True,
                        help="config key the readings belong to")
    record.add_argument("--from-junit", metavar="FILE",
                        help="ctest --output-junit XML")
    record.add_argument("--from-measure", metavar="FILE",
                        help="runner --record-out MEASURE file")
    record.add_argument("--form", help="measurement form (default per "
                                       "source)")
    caps_p = sub.add_parser("caps", help="print one entry's cap and "
                                         "budget")
    caps_p.add_argument("--table", help="reference table (default "
                                        "tests/gate_time_reference.txt)")
    caps_p.add_argument("config")
    caps_p.add_argument("entry")
    caps_p.add_argument("--work-factor", type=float, default=1.0)
    caps_p.add_argument("--scalar", type=float, default=1.0)
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
