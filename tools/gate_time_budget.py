# gate_time_budget.py -- guard three's post-hoc budget check for one
# finished ctest leg (guard ten's form refusal and the D-8 self-report
# cross-check ride along), the verdict body gate_time_guards.py kept
# beside the other guards until the source-size guard asked for a
# sibling of its own (docs/dev/gate_timing_design.md). It returns
# (findings, detail): detail lines are report lines, not findings. The
# row model and the single caps computation live in
# gate_time_table.py (D-5); the shared universe readers stay in
# gate_time_guards.py. Stdlib only; run with -I.

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_table import (DEFAULT_WORK_FACTOR, SAFETY_MULTIPLIER,
                             caps_values, form_token, resolve_scoped_row)


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
        elif (row.ref_work != DEFAULT_WORK_FACTOR
              and ROUNDS_TOKEN_RE.search((reports or {}).get(name, ""))):
            #A coarse row cannot pair a rounds count with one entry (it
            #reads one measurement for the whole scope), so it must not
            #charge plain units of reference work: an entry reporting
            #rounds under such a row is a named refusal demanding the
            #entry's own row, not a silently skipped assertion (D-8).
            findings.append(
                "SELFREPORT %s: rounds=<n> reading priced by coarse row "
                "%s (ref_work=%d) -- the self-report pairing needs the "
                "entry's own row (D-8)"
                % (name, row.entry, row.ref_work))
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
