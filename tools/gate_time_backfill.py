# gate_time_backfill.py -- the findings rules behind the check-schedule
# subcommand (docs/dev/gate_timing_design.md guards six/nine plus guard
# eight's perf-class verdict; the subcommand name and its parser stay
# registered by gate_time_schedule, which dispatches here exactly as it
# dispatches check-carrier into gate_time_carrier, R-28). The rules
# answer one question in five ways: is every entry a scheduled round
# skips still carried somewhere -- a family claim or a reason column,
# the -R/-E pair ctest itself resolves, the e2e subset formula, a live
# family src glob, and the perf registration difference against its
# priced rows. Stdlib only; run with -I.

import fnmatch
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_guards import read_name_set, read_perf_names
from gate_time_table import (CONFIG_KEYS, SENTINEL_MARK, SLOW_MIN_REF_S,
                             load_table)
from gate_time_timeouts import read_timeouts, show_only_json
from test_impact import _parse_families


def run(args, table, verdict):
    """The check-schedule runner (the shape gate_time_schedule.dispatch
    injects: resolved table path, the entry's verdict callback)."""
    for flag, config in (("--config", args.config),
                         ("--e2e-config", args.e2e_config)):
        if config is not None and config not in CONFIG_KEYS:
            return verdict(["CHECK-SCHEDULE %s: %s names an unknown "
                            "config key (want one of %s)"
                            % (flag, config, ", ".join(CONFIG_KEYS))])
    if args.e2e_config and not args.e2e_manifest:
        return verdict(["CHECK-SCHEDULE: --e2e-config needs --e2e-manifest "
                        "(the e2e universe is the manifest)"])
    # Membership and the baseline rows are the schedule axis' single
    # judgment (D-17), imported here rather than at module head:
    # gate_time_schedule dispatches into this module, so a head import
    # would be circular (the same shape as gate_time_carrier's, R-28).
    from gate_time_schedule import baseline_rows, slow_names
    rows = load_table(table)
    slow = set(slow_names(rows))
    baseline = baseline_rows(rows)
    families = _parse_families(Path(args.families).read_text(
        encoding="utf-8"))
    findings = _phantom_findings(families, args.repo_root)
    findings += _claim_findings(rows, families, args.config, slow, baseline)
    findings += _backfill_findings(args, rows)
    if args.perf_names:
        findings += _perf_names_findings(args, rows)
    if args.e2e_config:
        findings += _claim_findings(rows, families, args.e2e_config, slow,
                                    baseline)
        findings += _e2e_findings(args, rows)
    return verdict(findings)


def _claim_findings(rows, families, config, slow, baseline):
    """Guard six's claim half for one config: every row of a slow member
    (D-16: baseline row above SLOW_MIN_REF_S) must own 甲 (a family with
    at least one src rule whose unit or e2e fragment re.search-hits the
    entry name -- the family, not the row, carries the recovery) or 乙
    (a non-empty reason column naming where the entry still runs): an
    exception nobody carries is an entry that silently stops running.
    `slow` is the membership export and `baseline` the classifying rows,
    so the failure can quote the measured seconds instead of the deleted
    category column. Findings name the entry."""
    findings = []
    src_rules, _ignore, unit_by, e2e_by = families
    claiming = {family for _glob, family in src_rules}
    for row in rows:
        if row.config != config or row.entry not in slow:
            continue
        if row.reason.strip():
            continue
        if any(family in claiming
               and (re.search(unit_by.get(family, "(?!x)x"), row.entry)
                    or re.search(e2e_by.get(family, "(?!x)x"), row.entry))
               for family in unit_by.keys() | e2e_by.keys()):
            continue
        findings.append(
            "SCHEDULE %s %s: slow member (%.2f s on its baseline row, "
            "above the %d s gate) with neither a family claim (甲: a "
            "src-ruled family whose unit/e2e fragment hits the name) nor "
            "a reason column (乙) -- an exception nobody carries is an "
            "entry that silently stops running"
            % (config, row.entry, baseline[row.entry].ref_s,
               SLOW_MIN_REF_S))
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
    # Membership is the schedule axis' single judgment (see run).
    from gate_time_schedule import slow_names
    required = [name for name in slow_names(rows)
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
    derive_e2e_subset's formula output exactly -- a dropped explicit
    fast line silently shrinks every future round, and an extra line
    prices work the formula never selected. Findings name the line."""
    # The selection formula is the schedule axis' one site (see run).
    from gate_time_schedule import derive_e2e_subset
    manifest = Path(args.e2e_manifest).read_text(
        encoding="utf-8").splitlines()
    expected = derive_e2e_subset(manifest, rows, args.e2e_config,
                                 args.e2e_mapping)
    if not args.derived_e2e:
        return []
    written = {line.strip() for line in Path(args.derived_e2e).read_text(
        encoding="utf-8").splitlines() if line.strip()}
    findings = ["SCHEDULE %s: the derived e2e subset is missing the round "
                "line %r (explicit fast rows and mapping hits only)"
                % (args.e2e_config, line)
                for line in expected if line not in written]
    findings += [
        "SCHEDULE %s: the derived e2e subset holds %r, which the set "
        "formula does not select" % (args.e2e_config, line)
        for line in sorted(written - set(expected))]
    return findings


def _perf_names_findings(args, rows):
    """Guard eight, single source (R-61/D-17): the ON/OFF registration
    difference is the one truth about the perf class; the table only
    prices it. Two verdicts remain, neither consulting a category -- a
    difference name with no priced row would run in the regular legs
    unpriced (the priced set is the ctest-Release rows not marked
    sentinel=yes: D-3 pins that config's registration set to the perf
    names union the sentinel names and the sentinel column marks the
    latter, so one predicate restores the former without a new marker),
    and a difference name in a round's run enumeration means that
    round's -E lost the perf class. The old "table row missing from the
    difference -- it runs in no leg" direction is deleted: under D-16 an
    unpriced perf row simply runs in every regular leg, and guard one's
    EXTRA ctest-Release verdict owns that direction once Task 10 Step 5
    wires it (R-43); until then its bound is the two rows' combined 7.44
    s of extra per-round runtime. The file is read through the shared
    perf-name roster reader (read_perf_names, spec D-17's format
    contract): print-perf-names' '|'-joined value line, one name per
    line, or that command's raw stdout redirect with its trailing 'GATE '
    verdict line -- always the ON/OFF DIFFERENCE, never the ON
    enumeration."""
    # The perf config key is pinned by D-3 in the schedule module
    # (see run).
    from gate_time_schedule import PERF_CONFIG
    names = read_perf_names(args.perf_names)
    priced = {row.entry for row in rows
              if row.config == PERF_CONFIG and row.sentinel != SENTINEL_MARK}
    findings = [
        "PERF: the registration difference holds %s, which has no priced "
        "ctest-Release row (no row, or one marked sentinel=yes) -- it "
        "would run in the regular legs unpriced" % name
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
