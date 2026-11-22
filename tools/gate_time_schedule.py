# gate_time_schedule.py -- the schedule axis (docs/dev/
# gate_timing_design.md D-7): the reference table's category column is
# the single source for which entries a scheduled round skips.
# print-slow builds a config's -E regex from the 类别!=常规 rows and
# subtracts whatever the leg's own -R regex re.search-hits first (ctest
# -E beats -R, F-12: an entry both mapped and slow must not be silently
# swallowed); check-schedule enforces guard six (every 类别!=常规 row
# owns a recovery claim -- 甲 a family whose unit/e2e fragment hits the
# entry over a live src rule, or 乙 a filled reason column -- and every
# >SLOW_MIN_REF_S 常规 row states its reason, with the backfill
# equivalence ctest itself computes and the e2e set formula "explicit-
# 常规 rows union mapping hits, manifest order, deduped") and guard
# nine (every family src glob hits at least one existing repo file; a
# phantom glob would silently map a touched tree onto nothing);
# derive-e2e-subset applies that e2e formula for the T0 wiring, so the
# selection lives in exactly one place. print-perf-names enumerates the
# perf class as the name difference of two scratch configures (flag ON
# versus OFF, the seed cache's shape forwarded) so the set comes from
# the registration itself, and check-carrier's guard eleven (body in
# gate_time_carrier) pairs every 类别!=常规 row with a leg that ran it
# under the row's own config key. The CLI entry and the shared verdict
# glyph stay in gate_time_check.py: this module registers its
# subcommand parsers and executes them through the injected verdict
# callback (R-28/R-29). Stdlib only; run with -I.

import argparse
import fnmatch
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gate_time_carrier
from gate_time_guards import read_name_set
from gate_time_table import (CONFIG_KEYS, PERF_CATEGORY, REPO_ROOT,
                             REGULAR_CATEGORY, SLOW_MIN_REF_S, load_table)
from gate_time_timeouts import cmake_tools, read_timeouts, show_only_json
from test_impact import _parse_families

# The subcommands this module owns; the entry maps these names here.
# check-carrier's check body lives in gate_time_carrier (the entry
# keeps its single hook and this file its registration: R-28).
COMMANDS = ("print-slow", "derive-e2e-subset", "check-schedule",
            "print-perf-names", "check-carrier")


def slow_names(rows, config):
    """The config's exception entries (类别!=常规), table order: the
    membership every scheduled round skips unless its own -R maps one
    back in."""
    return [row.entry for row in rows
            if row.config == config and row.category != REGULAR_CATEGORY]


def slow_regex(rows, config, keep_matched=None):
    """The joined -E regex for a config (sorted, so one reference table
    yields one byte-stable exclusion). keep_matched is the leg's -R
    regex VERBATIM (the t0() mapping, not a name list: passing the
    regex keeps the filtering logic in this one place): every exception
    entry it re.search-hits -- ctest's partial-match semantics, the
    same F-12 measures -- is subtracted before joining, because ctest
    requires "hits -R and misses -E" and an unsubtracted mapped entry
    would vanish from the round silently."""
    names = set(slow_names(rows, config))
    if keep_matched:
        names = {name for name in names
                 if not re.search(keep_matched, name)}
    return "|".join(sorted(names))


def derive_e2e_subset(lines, rows, config, mapping):
    """This round's e2e lines under D-7's formula: the manifest lines
    that carry their own explicit 常规 row under the leg's config
    (the default row prices budgets for unlisted lines but never joins
    the subset -- that would pull the full manifest into every T0
    round, and the wall-clock gain comes from sharding, not from
    category filtering), unioned with the lines the mapping's e2e
    regex re.search-hits, in manifest order, deduped. Comments and
    blank lines are skipped; each line is emitted verbatim (entry and
    expected-exit columns), so the wiring can feed the file straight
    to the runner."""
    regular = {row.entry for row in rows
               if row.config == config and row.category == REGULAR_CATEGORY
               and row.entry != "default"}
    subset = []
    seen = set()
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        entry = line.split()[0]
        hit = mapping and re.search(mapping, entry)
        if entry in regular or hit:
            if entry not in seen:
                seen.add(entry)
                subset.append(line)
    return subset


def _claim_findings(rows, families, config):
    """Guard six's claim half for one config: every 类别!=常规 row must
    own 甲 (a family with at least one src rule whose unit or e2e
    fragment re.search-hits the entry name -- the family, not the row,
    carries the recovery) or 乙 (a non-empty reason column naming where
    the entry still runs); and every 常规 row above SLOW_MIN_REF_S with
    an empty reason column is an unexamined schedule decision. Both
    findings name the entry."""
    findings = []
    src_rules, _ignore, unit_by, e2e_by = families
    claiming = {family for _glob, family in src_rules}
    for row in rows:
        if row.config != config:
            continue
        if row.category == REGULAR_CATEGORY:
            if row.ref_s > SLOW_MIN_REF_S and not row.reason.strip():
                findings.append(
                    "SCHEDULE %s %s: reference %.2f s exceeds "
                    "SLOW_MIN_REF_S (%d s) yet the row stays %s with an "
                    "empty reason column -- record the schedule decision "
                    "or re-classify it"
                    % (config, row.entry, row.ref_s, SLOW_MIN_REF_S,
                       REGULAR_CATEGORY))
            continue
        if row.reason.strip():
            continue
        if any(family in claiming
               and (re.search(unit_by.get(family, "(?!x)x"), row.entry)
                    or re.search(e2e_by.get(family, "(?!x)x"), row.entry))
               for family in unit_by.keys() | e2e_by.keys()):
            continue
        findings.append(
            "SCHEDULE %s %s: 类别=%s with neither a family claim (甲: a "
            "src-ruled family whose unit/e2e fragment hits the name) nor "
            "a reason column (乙) -- an exception nobody carries is an "
            "entry that silently stops running"
            % (config, row.entry, row.category))
    return findings


def _repo_files(repo_root):
    """Every repo-relative posix file path under the root (guard nine's
    universe): .git pruned everywhere, the root's build trees and temp
    pruned as well -- they hold generated copies a glob could hit
    without the source surface existing."""
    paths = []
    for dir_index, (dir_path, dir_names, file_names) in enumerate(
            os.walk(repo_root)):
        dir_names[:] = [d for d in dir_names if d != ".git"
                        and not (dir_index == 0
                                 and (d == "build" or d == "temp"
                                      or d.startswith("build-")))]
        base = Path(dir_path)
        for name in file_names:
            paths.append(
                (base / name).relative_to(repo_root).as_posix())
    return paths


def _phantom_findings(families, repo_root):
    """Guard nine: every family src glob must hit at least one existing
    repo file (D-7 caught exactly such a phantom -- a src/vm/
    native_tensor* glob for a tree that never existed there -- while
    the family still LOOKED wired). fnmatch semantics match the
    impact mapper's own, so a glob that guards nine blesses is a glob
    the mapping will actually fire on."""
    src_rules, _ignore, _unit, _e2e = families
    files = _repo_files(Path(repo_root))
    findings = []
    for glob, family in src_rules:
        if not any(fnmatch.fnmatch(path, glob) for path in files):
            findings.append(
                "SCHEDULE: family %s src glob %s matches no file under "
                "%s (a phantom glob maps a touched tree onto nothing)"
                % (family, glob, repo_root))
    return findings


def _backfill_findings(args, rows):
    """Guard six's equivalence half: hand ctest the leg's -R/-E pair as
    it will run it and every mapping-hit exception entry must be in the
    --show-only output -- the exclusion side of the wiring has exactly
    one way to be wrong (F-12's swallow), and this replays ctest's own
    answer instead of re-deriving it. --show-only compares a saved
    document; --tree enumerates the configured tree live (the -E set
    print-slow derives from the same table, so a live tree holds every
    entry the pair does not exclude)."""
    if not args.mapping:
        return []
    required = [name for name in slow_names(rows, args.config)
                if re.search(args.mapping, name)]
    if not required:
        return []
    if args.show_only:
        names = read_name_set(args.show_only)
    elif args.tree:
        names = set(read_timeouts(show_only_json(Path(args.tree))))
    else:
        return []
    return [
        "SCHEDULE %s: %s hits the mapping AND sits in the slow set; ctest "
        "-E beats -R (F-12), so print-slow must subtract it from the -E "
        "regex -- it is missing from the --show-only run"
        % (args.config, name)
        for name in sorted(required) if name not in names]


def _e2e_findings(args, rows):
    """Guard six's e2e side: the wiring's written subset file must equal
    derive_e2e_subset's formula output exactly -- a dropped explicit-
    常规 line silently shrinks every future round, and an extra line
    prices work the formula never selected. Findings name the line."""
    manifest = Path(args.e2e_manifest).read_text(
        encoding="utf-8").splitlines()
    expected = derive_e2e_subset(manifest, rows, args.e2e_config,
                                 args.e2e_mapping)
    if not args.derived_e2e:
        return []
    written = {line.strip() for line in Path(args.derived_e2e).read_text(
        encoding="utf-8").splitlines() if line.strip()}
    findings = ["SCHEDULE %s: the derived e2e subset is missing the round "
                "line %r (explicit-常规 rows and mapping hits only)"
                % (args.e2e_config, line)
                for line in expected if line not in written]
    findings += [
        "SCHEDULE %s: the derived e2e subset holds %r, which the set "
        "formula does not select" % (args.e2e_config, line)
        for line in sorted(written - set(expected))]
    return findings


def _perf_names_findings(args, rows):
    """Guard eight: the ON/OFF registration difference and the table's
    性能 rows are two independent views of one set -- both directions of
    the set difference must come up empty (a 性能 row whose flag
    vanished would run in no leg, a flag-only entry would run unpriced)
    -- and a perf entry may appear in NO T0/T1 run enumeration: the
    round that still lists one lost its -E. The file is print-perf-
    names' value line verbatim ('|' or newline separated)."""
    text = Path(args.perf_names).read_text(encoding="utf-8")
    names = {name for name in re.split(r"[|\r\n]+", text) if name}
    priced = {row.entry for row in rows if row.category == PERF_CATEGORY}
    findings = [
        "PERF: the %s row %s is missing from the ON/OFF registration "
        "difference -- it runs in no leg" % (PERF_CATEGORY, name)
        for name in sorted(priced - names)]
    findings += [
        "PERF: the registration difference holds %s, which has no %s "
        "row -- it would run unpriced" % (name, PERF_CATEGORY)
        for name in sorted(names - priced)]
    if not (args.show_only or args.tree):
        return findings
    if args.show_only:
        running = read_name_set(args.show_only)
    else:
        running = set(read_timeouts(show_only_json(Path(args.tree))))
    findings += [
        "PERF: the round's run set still holds %s -- its -E lost the "
        "perf class, which belongs to the T2-perf leg alone" % name
        for name in sorted(names & running)]
    return findings


def _cmd_print_slow(args, table, verdict):
    if args.config not in CONFIG_KEYS:
        return verdict(["PRINT-SLOW %s: unknown config key (want one of "
                        "%s)" % (args.config, ", ".join(CONFIG_KEYS))])
    print(slow_regex(load_table(table), args.config, args.keep_matched))
    return verdict([])


def _cmd_derive_e2e_subset(args, table, verdict):
    if args.config not in CONFIG_KEYS:
        return verdict(["DERIVE-E2E %s: unknown config key (want one of "
                        "%s)" % (args.config, ", ".join(CONFIG_KEYS))])
    lines = Path(args.manifest).read_text(encoding="utf-8").splitlines()
    for line in derive_e2e_subset(lines, load_table(table), args.config,
                                  args.mapping):
        print(line)
    return verdict([])


def _cmd_check_schedule(args, table, verdict):
    for flag, config in (("--config", args.config),
                         ("--e2e-config", args.e2e_config)):
        if config is not None and config not in CONFIG_KEYS:
            return verdict(["CHECK-SCHEDULE %s: %s names an unknown "
                            "config key (want one of %s)"
                            % (flag, config, ", ".join(CONFIG_KEYS))])
    if args.e2e_config and not args.e2e_manifest:
        return verdict(["CHECK-SCHEDULE: --e2e-config needs --e2e-manifest "
                        "(the e2e universe is the manifest)"])
    rows = load_table(table)
    families = _parse_families(Path(args.families).read_text(
        encoding="utf-8"))
    findings = _phantom_findings(families, args.repo_root)
    findings += _claim_findings(rows, families, args.config)
    findings += _backfill_findings(args, rows)
    if args.perf_names:
        findings += _perf_names_findings(args, rows)
    if args.e2e_config:
        findings += _claim_findings(rows, families, args.e2e_config)
        findings += _e2e_findings(args, rows)
    return verdict(findings)


def _cmd_print_perf_names(args, table, verdict):
    """D-6: the perf name set is the registration difference, never a
    table consultation. The source tree is configured twice under
    independent scratch directories -- flag ON first, then OFF -- each
    repeating the --seed-tree's cache shape (generator, platform,
    toolset and the non-INTERNAL cache entries, minus the perf flag,
    whose one spelling per side is the pair's whole point) so both
    configures register like the real sweep tree. The printed value is
    the --show-only name difference ON minus OFF, one '|'-joined line
    (print-slow's shape); the seed tree is only read -- the sweep
    tree's cache stays untouched."""
    seed = Path(args.seed_tree)
    cache = seed / "CMakeCache.txt"
    if not cache.is_file():
        return verdict(["PRINT-PERF-NAMES: %s is missing -- the scratch "
                        "configures repeat the --seed-tree's cache "
                        "shape (generator and cache flags) to register "
                        "like the real sweep tree" % cache])
    try:
        cmake, _ctest = cmake_tools()
    except ValueError as exc:
        return verdict([str(exc)])
    values = gate_time_carrier.cache_values(cache)
    forward = []
    for flag, entry in (("-G", "CMAKE_GENERATOR"),
                        ("-A", "CMAKE_GENERATOR_PLATFORM"),
                        ("-T", "CMAKE_GENERATOR_TOOLSET")):
        value = values.get(entry, ("", ""))[1]
        if value:
            forward += [flag, value]
    for name, (type_name, value) in sorted(values.items()):
        if (type_name != "INTERNAL" and not name.startswith("_")
                and name != "NLANG_PERF_TESTS"):
            forward.append("-D%s=%s" % (name, value))
    names = {}
    for side in ("ON", "OFF"):
        tree = Path(args.scratch) / side
        proc = subprocess.run(
            [cmake, "-S", str(Path(args.source_tree)), "-B", str(tree)]
            + forward + ["-DNLANG_PERF_TESTS=%s" % side],
            capture_output=True, text=True, encoding="utf-8")
        if proc.returncode != 0:
            return verdict([
                "PRINT-PERF-NAMES: the %s scratch configure failed with "
                "rc=%d: %s" % (side, proc.returncode,
                               (proc.stderr or "").strip()[-400:])])
        names[side] = set(read_timeouts(show_only_json(tree)))
    print("|".join(sorted(names["ON"] - names["OFF"])))
    return verdict([])


_COMMAND_RUNNERS = {
    "print-slow": _cmd_print_slow,
    "derive-e2e-subset": _cmd_derive_e2e_subset,
    "check-schedule": _cmd_check_schedule,
    "print-perf-names": _cmd_print_perf_names,
    "check-carrier": gate_time_carrier.run,
}


def dispatch(args, table, verdict):
    """The entry's execution hook (R-29): resolve the subcommand to its
    runner; the table path is the entry's resolved default, the verdict
    glyph is the entry's callback."""
    runner = _COMMAND_RUNNERS.get(args.command)
    if runner is None:
        raise ValueError("gate_time_schedule: unknown command %r"
                         % args.command)
    return runner(args, table, verdict)


def add_subparsers(sub):
    """The entry's registration hook (R-29): this module owns its
    subcommand parsers outright -- the entry file sits at the 500-line
    bound and only maps names to these hooks. --table repeats here
    because importing the entry's shared parent parser would make the
    import circular."""
    table_arg = argparse.ArgumentParser(add_help=False)
    table_arg.add_argument("--table", help="reference table (default "
                                           "tests/gate_time_reference.txt)")
    slow_p = sub.add_parser("print-slow", parents=[table_arg],
                            help="D-7 wiring input: the config's "
                                 "类别!=常规 -E regex")
    slow_p.add_argument("config", help="config key the leg judges under")
    slow_p.add_argument("--keep-matched", metavar="REGEX",
                        help="the leg's -R regex verbatim; exception "
                             "entries it re.search-hits are subtracted "
                             "before joining (F-12)")
    derive_p = sub.add_parser("derive-e2e-subset", parents=[table_arg],
                              help="this round's e2e lines: explicit-常"
                                   "规 rows ∪ mapping hits, manifest "
                                   "order")
    derive_p.add_argument("config", help="e2e config key the leg judges "
                                         "under")
    derive_p.add_argument("--manifest", required=True, metavar="FILE",
                          help="the manifest the subset derives from")
    derive_p.add_argument("--mapping",
                          help="the mapping's e2e regex (union side)")
    sched_p = sub.add_parser("check-schedule", parents=[table_arg],
                             help="guards six/nine: category claims, the "
                                  "backfill equivalence, phantom src "
                                  "globs, the e2e set formula")
    sched_p.add_argument("--config", required=True,
                         help="ctest config key to judge")
    sched_p.add_argument("--families",
                         default=str(REPO_ROOT / "tools"
                                     / "test_families.txt"),
                         help="the impact-mapping families file (guard "
                              "nine + 甲 claims)")
    sched_p.add_argument("--repo-root", default=str(REPO_ROOT),
                         help="repo root the src globs are checked "
                              "against")
    sched_p.add_argument("--mapping",
                         help="the leg's -R regex (backfill equivalence "
                              "side)")
    sched_p.add_argument("--show-only", metavar="FILE",
                         help="saved ctest --show-only=json-v1 document "
                              "holding the pair's run set")
    sched_p.add_argument("--tree", help="configured tree to enumerate "
                                        "live instead of a saved document")
    sched_p.add_argument("--e2e-config",
                         help="e2e config key whose rows the e2e leg "
                              "judges under")
    sched_p.add_argument("--e2e-manifest", metavar="FILE",
                         help="the e2e manifest the subset derives from")
    sched_p.add_argument("--e2e-mapping",
                         help="the mapping's e2e regex (formula union "
                              "side)")
    sched_p.add_argument("--derived-e2e", metavar="FILE",
                         help="the wiring's written subset file to "
                              "compare against the formula")
    sched_p.add_argument("--perf-names", metavar="FILE",
                         help="print-perf-names' ON/OFF registration "
                              "difference (guard eight: equal to the "
                              "性能 rows in both directions, absent "
                              "from the round's run set)")
    perf_p = sub.add_parser("print-perf-names", parents=[table_arg],
                            help="D-6: the perf class as the name "
                                 "difference of an ON and an OFF scratch "
                                 "configure")
    perf_p.add_argument("--seed-tree", required=True,
                        help="configured tree whose cache shape (gener-"
                             "ator and cache flags) the scratch "
                             "configures repeat")
    perf_p.add_argument("--scratch", required=True,
                        help="directory holding the two scratch trees "
                             "(<scratch>/ON, <scratch>/OFF)")
    perf_p.add_argument("--source-tree", default=str(REPO_ROOT),
                        help="source tree to configure (-S, default the "
                             "repo root)")
    gate_time_carrier.add_subparsers(sub, table_arg)
