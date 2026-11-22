# gate_time_schedule.py -- the schedule axis (docs/dev/
# gate_timing_design.md D-7 as re-derived by D-16/D-17): an entry's
# reference seconds on its baseline row are the single source for which
# entries a scheduled round skips (slow_names/baseline_rows here derive
# the one global slow set from the SLOW_MIN_REF_S threshold -- this
# module is the only fast/slow judgment site, D-17). print-slow
# builds a leg's -E regex from that set and subtracts whatever the leg's
# own -R regex re.search-hits first (ctest -E beats -R, F-12: an entry
# both mapped and slow must not be silently swallowed); check-schedule
# enforces guard six (every row of a slow member owns a recovery claim
# -- 甲 a family whose unit/e2e fragment hits the entry over a live src
# rule, or 乙 a filled reason column -- with the backfill equivalence
# ctest itself computes and the e2e set formula "explicit fast rows
# union mapping hits, manifest order, deduped") and guard nine (every
# family src glob hits at least one existing repo file; a phantom glob
# would silently map a touched tree onto nothing); derive-e2e-subset
# applies that e2e formula for the T0 wiring, so the selection lives in
# exactly one place. print-perf-names enumerates the perf set as the
# name difference of two scratch configures (flag ON versus OFF, the
# seed cache's shape forwarded) so the set comes from the registration
# itself. The two coverage checks keep their verdict bodies in their own
# modules -- check-schedule's findings rules in gate_time_backfill,
# check-carrier's guard eleven in gate_time_carrier -- each pairing
# table rows with the legs that must have run them (R-28). The CLI entry
# and the
# shared verdict glyph stay in gate_time_check.py: this module registers
# its subcommand parsers and executes them through the injected verdict
# callback (R-28/R-29). Stdlib only; run with -I.

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gate_time_backfill
import gate_time_carrier
from gate_time_table import (CONFIG_KEYS, MANIFEST_SCOPES, REPO_ROOT,
                             SLOW_MIN_REF_S, load_table)
from gate_time_timeouts import cmake_tools, read_timeouts, show_only_json

# The subcommands this module owns; the entry maps these names here.
# check-schedule's findings rules live in gate_time_backfill and
# check-carrier's check body in gate_time_carrier (the entry keeps its
# single hook and this file its registration: R-28).
COMMANDS = ("print-slow", "derive-e2e-subset", "check-schedule",
            "print-perf-names", "check-carrier")

# The perf registration and the ctest-Release priced-set comparison
# (guard eight) are pinned to that config key by D-3.
PERF_CONFIG = "ctest-Release"

# The config keys that decide one entry name's fast/slow membership
# (D-16), most representative first: the ctest-Debug row when the entry
# runs in the default tree, else the ctest-IDE row of an entry registered
# only in the IDE tree, else the e2e-Debug row. ASan and Release rows
# price the same work under a more expensive build and never classify.
BASELINE_CONFIGS = ("ctest-Debug", "ctest-IDE", "e2e-Debug")


def baseline_rows(rows):
    """The classifying row of every entry name (D-16): the first hit in
    BASELINE_CONFIGS order. The coarse-scope rows (`default`, manifest
    scopes) price a run of entries rather than one entry, and ASan and
    Release rows price a different build -- none of them classify, so
    they never appear here."""
    baseline = {}
    for row in rows:
        if row.entry == "default" or row.entry in MANIFEST_SCOPES:
            continue
        if row.config not in BASELINE_CONFIGS:
            continue
        prev = baseline.get(row.entry)
        if prev is None or (BASELINE_CONFIGS.index(row.config)
                            < BASELINE_CONFIGS.index(prev.config)):
            baseline[row.entry] = row
    return baseline


def slow_names(rows):
    """The slow member names (D-16/D-17): an entry name is slow exactly
    when its baseline row's reference seconds exceed SLOW_MIN_REF_S
    (strictly). The list is one per repo -- every config's regular legs
    subtract the same names, and only the mapped closeout legs re-run
    them. This is the single membership judgment site (D-17): no consumer
    derives fast/slow from a row of its own config."""
    return [row.entry for row in baseline_rows(rows).values()
            if row.ref_s > SLOW_MIN_REF_S]


def slow_regex(rows, config, keep_matched=None):
    """The joined -E regex for a leg (sorted, so one reference table
    yields one byte-stable exclusion). The membership is the one global
    slow set (D-16) -- every config's regular legs subtract the same
    names -- so `config` is the leg's label in the CLI shape, not a
    filter. keep_matched is the leg's -R regex VERBATIM (the t0()
    mapping, not a name list: passing the regex keeps the filtering
    logic in this one place): every slow member it re.search-hits --
    ctest's partial-match semantics, the same F-12 measures -- is
    subtracted before joining, because ctest requires "hits -R and
    misses -E" and an unsubtracted mapped entry would vanish from the
    round silently."""
    names = set(slow_names(rows))
    if keep_matched:
        names = {name for name in names
                 if not re.search(keep_matched, name)}
    return "|".join(sorted(names))


def derive_e2e_subset(lines, rows, config, mapping):
    """This round's e2e lines under D-7's formula (R-64 shape): a
    manifest line qualifies when BOTH hold -- it carries its own explicit
    (non-`default`) row under the leg's config (the candidate face: the
    `default` row prices budgets for unlisted lines but never joins the
    subset, that would pull the full manifest into every T0 round), and
    its name is not in the one global slow set (the fast/slow face, read
    from the baseline row per D-16 -- never from this leg's own config
    row, or an ASan or Release reading would decide membership). The set
    is unioned with the lines the mapping's e2e regex re.search-hits, in
    manifest order, deduped. Comments and blank lines are skipped; each
    line is emitted verbatim (entry and expected-exit columns), so the
    wiring can feed the file straight to the runner."""
    slow = set(slow_names(rows))
    explicit = {row.entry for row in rows
                if row.config == config and row.entry != "default"}
    subset = []
    seen = set()
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        entry = line.split()[0]
        hit = mapping and re.search(mapping, entry)
        if (entry in explicit and entry not in slow) or hit:
            if entry not in seen:
                seen.add(entry)
                subset.append(line)
    return subset


def _cmd_print_slow(args, table, verdict):
    if args.config not in CONFIG_KEYS:
        return verdict(["PRINT-SLOW %s: unknown config key (want one of "
                        "%s)" % (args.config, ", ".join(CONFIG_KEYS))])
    regex = slow_regex(load_table(table), args.config, args.keep_matched)
    print(regex)
    # The leg log has to name the members the regex excludes (D-17): a
    # bare pattern is unreadable in a log line. The roster rides on
    # stderr because stdout is the bash-captured -E regex at the wiring
    # and the GATE verdict glyphs also print on stdout -- a roster line
    # there would be concatenated into the captured pattern.
    members = regex.split("|") if regex else []
    sys.stderr.write("PRINT-SLOW %s excludes %d slow members: %s\n"
                     % (args.config, len(members),
                        ", ".join(members) if members else "(none)"))
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
    "check-schedule": gate_time_backfill.run,
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
                            help="D-16 wiring input: the global slow "
                                 "set (baseline row above "
                                 "SLOW_MIN_REF_S) as one -E regex")
    slow_p.add_argument("config", help="config key the leg judges under")
    slow_p.add_argument("--keep-matched", metavar="REGEX",
                        help="the leg's -R regex verbatim; slow members "
                             "it re.search-hits are subtracted before "
                             "joining (F-12)")
    derive_p = sub.add_parser("derive-e2e-subset", parents=[table_arg],
                              help="this round's e2e lines: explicit "
                                   "fast rows ∪ mapping hits, manifest "
                                   "order")
    derive_p.add_argument("config", help="e2e config key the leg judges "
                                         "under")
    derive_p.add_argument("--manifest", required=True, metavar="FILE",
                          help="the manifest the subset derives from")
    derive_p.add_argument("--mapping",
                          help="the mapping's e2e regex (union side)")
    sched_p = sub.add_parser("check-schedule", parents=[table_arg],
                             help="guards six/nine: slow-membership "
                                  "claims, the backfill equivalence, "
                                  "phantom src globs, the e2e set "
                                  "formula")
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
                         help="print-perf-names' value line: the ON/OFF "
                              "registration DIFFERENCE, not the ON "
                              "enumeration (guard eight: every name owns "
                              "a priced ctest-Release row, and no perf "
                              "entry sits in the round's run set)")
    perf_p = sub.add_parser("print-perf-names", parents=[table_arg],
                            help="D-6: the perf set as the name "
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
