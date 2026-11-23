# gate_time_measure.py -- measured readings in and the machine scale out:
# parse_measure_rows/parse_junit_rows turn the e2e runner's MEASURE
# records and ctest --output-junit documents into {name: (seconds, form)}
# readings, apply_record merges readings into a table refreshing ref_s
# and form only (the four human columns survive verbatim), calibrate_scale
# derives the machine scale as the sentinel ratio median (guard four),
# and sample_ctest/sample_e2e run the live sampling rounds those readings
# come from (docs/dev/gate_timing_design.md). The row model and the caps
# formula live in gate_time_table.py, the sentinel admission helpers in
# gate_time_guards.py, and the CLI entry is gate_time_check.py. Stdlib
# only; run with -I.

import os
import re
import statistics
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_time_guards import (sentinel_rows, sentinel_shortfall_finding)
from gate_time_table import (CONFIG_KEYS, DEFAULT_WORK_FACTOR,
                             MANIFEST_SCOPES, REPO_ROOT, SCALE_BAND_MAX,
                             SCALE_BAND_MIN, load_table, row_line)


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


def junit_cases(xml_text):
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


def junit_reports(xml_text):
    """The captured output channel of a ctest --output-junit document:
    {name: system-out text} for every <testcase> whose <system-out> holds
    non-empty text. CMake 3.25.2 writes a passing entry's stdout there
    (probed: a failing entry carries <failure> and <system-out> side by
    side), which is why the D-8 self-report cross-check reads reports
    from the same document the timings come from -- one file, one
    source. An entry without a report is absent from the map (the
    checker turns absence into "did not self-report")."""
    reports = {}
    for case in ET.fromstring(xml_text).iter("testcase"):
        name = case.get("name")
        if not name:
            continue
        parts = [node.text for node in case.iter("system-out")
                 if node.text]
        if parts:
            reports[name] = "\n".join(parts)
    return reports


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
    for name, status, secs in junit_cases(xml_text):
        if status != "run":
            continue
        if name not in measurements or secs > measurements[name][0]:
            measurements[name] = (secs, form)
    return measurements


def apply_record(table_path, config, measurements):
    """Merge measured readings into the table. Entry rows of `config`
    refresh ref_s and form only; ref_work, sentinel and reason keep their
    original tokens verbatim (guard six member), and every
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
        fields = stripped.split(None, 6)
        if (stripped and not stripped.startswith("#") and len(fields) >= 5
                and fields[0] == config and fields[1] in measured):
            secs, form = measurements[fields[1]]
            sentinel = fields[5] if len(fields) > 5 else ""
            reason = fields[6] if len(fields) > 6 else ""
            out.append(row_line(config, fields[1], secs, fields[3],
                                 form or fields[4], sentinel, reason))
            updated += 1
        else:
            out.append(line)
    added = []
    if not has_default:
        for name in sorted(set(measurements) - measured):
            secs, form = measurements[name]
            out.append(row_line(config, name, secs,
                                 str(DEFAULT_WORK_FACTOR),
                                 form or "unknown", "", ""))
            added.append(name)
    # Explicit newline="\n": the table stays LF on every platform (the
    # write_text(newline=) form needs Python 3.10, the trees' cached
    # Python3 is older).
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(out) + "\n")
    return updated, added


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
    sent = sentinel_rows(config, rows)
    shortfall = sentinel_shortfall_finding(config, len(sent))
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
        candidates.append(row_line(config, row.entry, secs, row.ref_work,
                                    row.form, row.sentinel, row.reason))
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


def sample_ctest(config, tree, names):
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


def sample_e2e(config, tools_tree, names, manifest_path):
    """Guard four sampling round for an e2e config (D-4): the manifest
    lines whose first column is a sentinel form a derived subset (the
    runner takes one manifest per run, and subsets are derived artifacts,
    never stored), run through the runner with --record-out in the legs'
    serial form. --config states this config's own table column so the
    sampling run never leans on the tool-path guess (an ASan sweep drives
    Debug binaries yet prices from the e2e-ASan rows); --timeout=120
    prices only row-less entries now and every sentinel carries its own
    row, so neither flag can shape a calibration reading (guard seven's
    MIN_CAP_S clause keeps sentinel references well inside their
    budgets)."""
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
             "--config=%s" % config.split("-", 1)[1],
             "--timeout=120", "--record-out", str(record)],
            cwd=str(runner.parent), env=env, capture_output=True,
            text=True, encoding="utf-8")
        if not record.is_file():
            raise ValueError("calibration e2e run against %s produced no "
                             "record file (rc=%d): are the tools built?"
                             % (tools_tree, proc.returncode))
        return parse_measure_rows(record.read_text(encoding="utf-8"))
