# gate_time_check.py -- the command-line entry for the reference timing
# table guards (docs/dev/gate_timing_design.md). The reference-table
# model and the single caps()/budget derivation site live in
# gate_time_table.py, the measurement parsing, the table recording and
# the calibration sampling rounds in gate_time_measure.py, the guard
# verdict bodies in gate_time_guards.py (guard five's registered-cap
# axis in its sibling gate_time_timeouts.py), and the schedule axis
# (print-slow, derive-e2e-subset, guards six and nine) in
# gate_time_schedule.py, which registers its own parsers and this file
# only maps the names to its runners (R-28/R-29); this module owns the
# argument parsing and the per-subcommand dispatch. Stdlib only; run
# with -I.
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
#   check-timeouts   guard five: every enumerated entry's registered ctest
#                    TIMEOUT equals the watchdog cap the table derives under
#                    the tree's own config-key set and configure-time work
#                    factor (the generated thresholds header), the max over
#                    the keys; red wording splits by configure freshness
#                    (stale: re-run configure, one line; fresh: a
#                    hand-written TIMEOUT override per entry, line-named
#                    when it sits in tests/CMakeLists.txt)
#   print-slow       the schedule-axis wiring input (D-7): the config's
#                    类别!=常规 entry names as one -E regex, minus
#                    whatever --keep-matched (the leg's -R regex) hits
#   derive-e2e-subset the round's e2e lines: explicit-常规 rows ∪ mapping
#                    hits, manifest order (the T0 selection, one site)
#   check-schedule   guards six/nine: category claims (甲/乙), the
#                    backfill equivalence ctest itself computes, the >
#                    SLOW_MIN_REF_S decision, the e2e set formula and
#                    phantom src globs
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
# Importable consumers (D-5) live in gate_time_table.py: caps(config,
# entry, work_factor=1.0, scalar=1.0) -> (budget_s, watchdog_s) and
# load_table(path).

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gate_time_schedule
from gate_time_guards import (check_budget, check_complete, check_constants,
                              check_orphans, enum_overrides, read_name_set,
                              run_ctest_enum, sentinel_rows,
                              sentinel_shortfall_finding)
from gate_time_measure import (apply_record, calibrate_scale, junit_cases,
                               junit_reports, parse_junit_rows,
                               parse_measure_rows, sample_ctest, sample_e2e)
from gate_time_table import (CONFIG_KEYS, CTEST_CONFIGS, DECISION_TREES,
                             DEFAULT_TABLE, DEFAULT_WORK_FACTOR,
                             REPO_ROOT, SCALE_BAND_MAX, SCALE_BAND_MIN,
                             caps, effective_rows, emit_cmake)
from gate_time_timeouts import (check_timeouts, expected_watchdogs,
                                override_site, read_timeouts,
                                show_only_json, threshold_inputs,
                                tree_is_fresh)


def _inputs(args, repo):
    """Derived universes for guards one/two: live --show-only enumerations
    per decision tree unless --enum pins a config to a dump file;
    --perf-on-enum (or --enum ctest-Release=FILE) feeds the ctest-Release
    universe; both e2e manifests default to the repo lists."""
    overrides = enum_overrides(args.enum)
    enums = {}
    for cfg, tree in DECISION_TREES.items():
        if cfg in overrides:
            enums[cfg] = read_name_set(overrides[cfg])
        else:
            enums[cfg] = run_ctest_enum(repo / tree)
    perf_on = None
    if args.perf_on_enum:
        perf_on = read_name_set(args.perf_on_enum)
    elif "ctest-Release" in overrides:
        perf_on = read_name_set(overrides["ctest-Release"])
    manifests = {
        "manifest.txt": read_name_set(
            args.manifest or repo / "tests" / "e2e" / "manifest.txt"),
        "examples_manifest.txt": read_name_set(
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


def _run_caps(args, table):
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


def _run_emit_cmake(args, table):
    # A bad work factor or config key raises before anything is
    # written: main's handler turns it into the ERROR + FAIL(error)
    # pair the CMake FATAL_ERROR quotes back (D-8), and the stale
    # generated file from the previous configure stays untouched.
    count = emit_cmake(args.configs, args.out,
                       work_factor=args.work_factor, table_path=table)
    print("EMIT-CMAKE entries=%d out=%s" % (count, args.out))
    return _verdict([])


def _run_record(args, table):
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


def _run_calibrate(args, table):
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
    rows = effective_rows(table)
    sent = sentinel_rows(args.config, rows)
    shortfall = sentinel_shortfall_finding(args.config, len(sent))
    if shortfall:
        return _verdict([shortfall])
    names = {row.entry for row in sent}
    if is_ctest:
        measurements = sample_ctest(args.config, args.tree, names)
    else:
        manifest = (args.manifest
                    or str(REPO_ROOT / "tests" / "e2e" / "manifest.txt"))
        measurements = sample_e2e(args.config, args.tools, names,
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


def _run_check_budget(args, table):
    if args.config not in CONFIG_KEYS:
        return _verdict(["CHECK-BUDGET %s: unknown config key (want "
                         "one of %s)" % (args.config,
                                         ", ".join(CONFIG_KEYS))])
    # One read of the document feeds both channels: the timings and the
    # per-entry <system-out> reports the D-8 self-check cross-checks
    # (the same file, so a report can never come from a different run).
    text = Path(args.junit).read_text(encoding="utf-8")
    findings, detail = check_budget(
        args.config, junit_cases(text), effective_rows(table),
        parallel=args.parallel, repeat=args.repeat,
        work_factor=args.work_factor, scalar=args.scalar,
        reports=junit_reports(text))
    # Detail lines (advisories, MISSING, triage listings) print in
    # both outcomes: they carry the context the verdict and the
    # findings alone would leave out.
    for line in detail:
        print(line)
    return _verdict(findings)


def _derived_inputs(args, table):
    """Rows plus the derived universes for guards one/two, with the ENUM
    progress lines shared by check-complete and check-orphans."""
    rows = effective_rows(table)
    enums, perf_on, manifests = _inputs(args, Path(args.repo))
    for cfg in sorted(enums):
        print("ENUM %s: %d entries" % (cfg, len(enums[cfg])))
    if perf_on is not None:
        print("ENUM ctest-Release: %d entries (NLANG_PERF_TESTS=ON form)"
              % len(perf_on))
    return rows, enums, perf_on, manifests


def _run_check_complete(args, table):
    rows, enums, perf_on, manifests = _derived_inputs(args, table)
    return _verdict(check_complete(rows, enums, manifests, perf_on)[1])


def _run_check_orphans(args, table):
    rows, enums, perf_on, manifests = _derived_inputs(args, table)
    ctest_union = set().union(*enums.values())
    return _verdict(check_orphans(rows, ctest_union, perf_on,
                                  manifests)[1])


def _run_check_constants(args, table):
    return _verdict(check_constants(effective_rows(table))[1])


def _run_check_timeouts(args, table):
    tree = Path(args.tree)
    # The header of the tree's own generated thresholds file gives the
    # config-key set and work factor the caps were derived from -- the
    # emitter's inputs, not the current shell's guess. A tree without
    # the header was never configured with the cap emitter, which is a
    # named disposition (sibling of tree_is_fresh's refusal), not a
    # read_text traceback.
    try:
        header = (tree / "gate_time_thresholds.cmake").read_text(
            encoding="utf-8")
    except FileNotFoundError:
        return _verdict([
            "TIMEOUTS: %s holds no gate_time_thresholds.cmake: the tree "
            "was not configured with the cap emitter -- re-run configure"
            % tree])
    configs, work_factor = threshold_inputs(header)
    text = (Path(args.show_only).read_text(encoding="utf-8")
            if args.show_only else show_only_json(tree))
    findings = check_timeouts(
        read_timeouts(text),
        expected_watchdogs(effective_rows(table), configs, work_factor),
        tree_is_fresh(tree, table), site_of=override_site)[1]
    return _verdict(findings)


def _dispatch(args):
    table = Path(args.table) if args.table else DEFAULT_TABLE
    if args.command == "caps":
        return _run_caps(args, table)
    if args.command == "emit-cmake":
        return _run_emit_cmake(args, table)
    if args.command == "record":
        return _run_record(args, table)
    if args.command == "calibrate":
        return _run_calibrate(args, table)
    if args.command == "check-budget":
        return _run_check_budget(args, table)
    if args.command == "check-constants":
        return _run_check_constants(args, table)
    if args.command == "check-timeouts":
        return _run_check_timeouts(args, table)
    if args.command == "check-complete":
        return _run_check_complete(args, table)
    if args.command in gate_time_schedule.COMMANDS:
        return gate_time_schedule.dispatch(args, table, _verdict)
    return _run_check_orphans(args, table)


def _parent_parsers():
    """One shared definition of --table: every subcommand takes the flag
    with the same name, default and help text, and the parents
    mechanism carries the same action into each parser, so the flag
    can never drift between subcommands. `derived` adds the
    derived-universe flags guards one/two consume."""
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
    return table_arg, derived


def _add_check_parsers(sub):
    """The coverage guards (one, two), the constant-band guard (seven)
    and the registered-cap guard (five)."""
    table_arg, derived = _parent_parsers()
    sub.add_parser("check-complete", parents=[derived],
                   help="guard one: coverage and sentinel counts")
    sub.add_parser("check-orphans", parents=[derived],
                   help="guard two: rows for entries that no longer exist")
    sub.add_parser("check-constants", parents=[table_arg],
                   help="guard seven: constant band self-consistency")
    timeouts_p = sub.add_parser("check-timeouts", parents=[table_arg],
                                help="guard five: registered ctest TIMEOUT "
                                     "properties against the table caps")
    timeouts_p.add_argument("--tree", required=True,
                            help="configured tree to audit (its generated "
                                 "gate_time_thresholds.cmake header gives "
                                 "the config-key set and work factor the "
                                 "caps were derived from)")
    timeouts_p.add_argument("--show-only", metavar="FILE",
                            help="saved ctest --show-only=json-v1 document "
                                 "(default: enumerate the tree live)")


def _add_record_parsers(sub):
    """The recording merge (guard six member: the four human columns
    survive verbatim)."""
    table_arg, _ = _parent_parsers()
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


def _add_derive_parsers(sub):
    """The caps readers and the machine-scale tools: caps, emit-cmake,
    calibrate and check-budget."""
    table_arg, _ = _parent_parsers()
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


def _build_parser():
    parser = argparse.ArgumentParser(
        description="Reference timing table guards (see the module "
                    "docstring).")
    sub = parser.add_subparsers(dest="command", required=True)
    # Creation order fixes the subcommand listing in --help; the groups
    # below build the parsers in the order the listing has always shown.
    _add_check_parsers(sub)
    _add_record_parsers(sub)
    _add_derive_parsers(sub)
    # The schedule axis (print-slow / derive-e2e-subset / check-schedule)
    # is registered and executed by gate_time_schedule.py (R-28/R-29);
    # this file only maps the names to its hooks.
    gate_time_schedule.add_subparsers(sub)
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
