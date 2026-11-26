# gate_time_cli.py -- the argparse tree of the gate-timing checker entry
# (gate_time_check.py): its sibling owns the subcommand bodies and the
# dispatch, this module owns the parser builders, so the two halves stay
# under the source-size guard and a flag added here reaches every
# subcommand through the parents mechanism (the --table definition is
# shared exactly once). The schedule-axis parsers are registered by
# gate_time_schedule.add_subparsers (R-28/R-29: the axis ships its own
# flags with its own runner). Stdlib only; run with -I.

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gate_time_schedule
from gate_time_table import DEFAULT_WORK_FACTOR, REPO_ROOT


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
    derived.add_argument("--perf-names",
                         help="ctest-Release perf half: the names from "
                              "print-perf-names, i.e. the NLANG_PERF_TESTS "
                              "ON/OFF registration difference, not the "
                              "full ON enumeration (R-61)")
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
    """The recording merge (guard six member: the three human columns
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
    emit_p.add_argument("--claims", metavar="FILE",
                        help="claim roster (the tools/gate_tiers.txt "
                             "claim lines): one set(GATE_RESOURCE_...) "
                             "per claimed entry, the variable the "
                             "add_test wrapper turns into the ctest "
                             "RESOURCE_LOCK property (D-11)")
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


def build_parser():
    parser = argparse.ArgumentParser(
        description="Reference timing table guards (see the module "
                    "docstring).")
    sub = parser.add_subparsers(dest="command", required=True)
    # Creation order fixes the subcommand listing in --help; the groups
    # below build the parsers in the order the listing has always shown.
    _add_check_parsers(sub)
    _add_record_parsers(sub)
    _add_derive_parsers(sub)
    # The schedule axis (print-slow / derive-e2e-subset / check-schedule
    # / print-perf-names / check-carrier) is registered and executed by
    # gate_time_schedule.py (R-28/R-29); this file only maps the names.
    gate_time_schedule.add_subparsers(sub)
    return parser
