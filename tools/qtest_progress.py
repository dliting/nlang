"""qtest_progress.py - timestamped mirror of a QTest run's progress.

A hang inside a QTest binary used to leave nothing behind on this
machine: GUI-subsystem QTest apps discard their result stream (stdout
redirect stays empty), and the `-o file,txt` log — although QTest
flushes it per completed function — is held with an exclusive lock for
the whole run, so nothing outside can tail it. Two channels fix that:

1. Live sidecar: a test class may override cleanup() to append one
   line per finished function ("<name> <epoch-ms>") to
   <qtest-log-dir>/<exe-stem>.live.txt (own fopen/fclose per write,
   so the file is readable between writes). This wrapper tails that
   file and stamps each line with clock time and the exact per-function
   duration (epoch delta between consecutive lines).
2. Post-mortem read: QTest unlocks the -o file when the process dies,
   so after a normal exit OR a timeout kill the wrapper reads it and
   reports the last completed function — a killed run answers "where
   did it stop: stuck in the function after <X>".

Usage (mirrors how tests/CMakeLists.txt invokes it):
    python qtest_progress.py --qtest-log <dir>/<name>.txt \
        --progress-log <dir>/<name>.progress.log [--timeout 170] \
        [--table FILE --config Debug --entry <ctest-name>] \
        -- <test.exe> [extra args...]

The wrapper appends `-o <qtest-log>,txt` to the child command line. On
`--timeout` it kills the whole child tree (taskkill /T on Windows, the
process group via killpg on POSIX) so a stuck debug child cannot
outlive the suite, then exits 124. Without `--timeout` the child runs
to completion and the child's exit code is
passed through unchanged.

With --config/--entry the wrapper also judges the finished run against
the entry's budget from the gate-time reference table (D-5: the same
rows and the single caps() site the configure-time TIMEOUT watchdog was
derived from; --timeout above stays the configure-time watchdog
headroom). A normal end over budget prints a BUDGET-EXCEEDED line on
this wrapper's stdout (ctest records it in the junit <system-out>, the
post-hoc checker dedups on it) and turns rc 0 into 1; a nonzero child
rc passes through untouched. The machine scale arrives through the leg
environment (NLANG_GATE_MACHINE_SCALE); a missing one prices with
SCALE_BAND_MAX and labels the run uncalibrated.
"""

import argparse
import datetime
import importlib.util
import os
import subprocess
import sys
import time

#The uncalibrated note and other budget wording are Chinese; ctest stores
#this wrapper's stdout into junit XML and the budget checker reads it
#back, so pin the text streams to UTF-8 (the zh-CN ANSI code page of a
#registered environment would otherwise encode those bytes as cp936).
sys.stdout.reconfigure(encoding='utf-8')
sys.stderr.reconfigure(encoding='utf-8')

POLL_SECONDS = 0.25


def clock_now() -> str:
    return datetime.datetime.now().strftime("%H:%M:%S")


def live_log_path(qtest_log: str, exe: str) -> str:
    return os.path.join(os.path.dirname(qtest_log),
                        os.path.splitext(os.path.basename(exe))[0]
                        + ".live.txt")


def parse_live_line(line: str):
    """'<name> <epoch-ms>' -> (name, epoch_ms); None when not parseable."""
    parts = line.split()
    if len(parts) != 2 or not parts[1].isdigit():
        return None
    return parts[0], int(parts[1])


def last_result_line(qtest_log: str) -> str:
    """The last 'VERDICT : Class::function()' line in the -o log."""
    try:
        with open(qtest_log, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return ""
    for line in reversed(lines):
        if " : " in line:
            return line.split(" : ", 1)[1].strip()
    return ""


def totals_line(qtest_log: str) -> str:
    try:
        with open(qtest_log, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return ""
    for line in reversed(lines):
        if line.startswith("Totals:"):
            return line.strip()
    return ""


def kill_tree(pid: int) -> None:
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"],
                       capture_output=True)
    else:
        #The child was started with start_new_session, so it leads its
        #own process group and killpg reaches the whole tree: a bare
        #os.kill would leave nvm/ndb grandchildren orphaned and alive.
        try:
            os.killpg(pid, 9)
        except OSError:
            pass


def _load_gate_time_table():
    """The leaf timing-table module (gate_time_table.py, next to this
    wrapper) loaded by path: caps() is the single budget/watchdog site
    every consumer shares (D-5), and loading the leaf keeps this
    standalone tool free of the checker package's sibling imports."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "gate_time_table.py")
    spec = importlib.util.spec_from_file_location("gate_time_table", path)
    #An absent tool file is missing input, not a silent fallback.
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _inline_budget(args):
    """This entry's budget and a one-line provenance note. A missing or
    unreadable budget never stays silent: budget is None and the note
    names the cause (no flags, no reference row, table unreadable, bad
    scale environment), which the wrapper logs next to START. The work
    factor scales the budget through the same D-1 site the configure-
    time TIMEOUT was derived from, so a doubled soak workload moves the
    budget and the watchdog together. Returns (budget_s, note)."""
    if not args.config or not args.entry:
        return None, "no --config/--entry given (budget judgment off)"
    table = args.table or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), os.pardir,
        "tests", "gate_time_reference.txt")
    try:
        table_module = _load_gate_time_table()
    except (OSError, ValueError, AttributeError) as exc:
        return None, f"gate_time_table load failed: {exc}"
    key = "ctest-" + args.config
    scalar_env = os.environ.get("NLANG_GATE_MACHINE_SCALE")
    try:
        scalar = (float(scalar_env) if scalar_env
                  else float(table_module.SCALE_BAND_MAX))
        factor = float(os.environ.get("NLANG_TEST_WORK_FACTOR")
                       or table_module.DEFAULT_WORK_FACTOR)
    except ValueError as exc:
        return None, f"bad NLANG_GATE_MACHINE_SCALE/NLANG_TEST_WORK_FACTOR: " \
                     f"{exc}"
    try:
        budget, _watchdog = table_module.caps(
            key, args.entry, work_factor=factor, scalar=scalar,
            table_path=table)
    except KeyError:
        return None, f"no reference row for {key} {args.entry}"
    except (ValueError, OSError) as exc:
        return None, str(exc)
    note = (f"{key} {args.entry} budget={budget:.2f} scale={scalar:g}"
            f" factor={factor:g}")
    if not scalar_env:
        note += " 未做本机校准（缺标量，按SCALE_BAND_MAX计价）"
    return budget, note


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--qtest-log", required=True,
                    help="the -o file the test binary writes")
    ap.add_argument("--progress-log", required=True,
                    help="timestamped mirror written by this wrapper")
    ap.add_argument("--timeout", type=float, default=None,
                    help="kill the child tree after this many seconds")
    ap.add_argument("--table", default=None,
                    help="gate-time reference table (default "
                         "tests/gate_time_reference.txt)")
    ap.add_argument("--config", default=None,
                    help="bare config segment of this tree "
                         "(Debug|IDE|ASan|Release)")
    ap.add_argument("--entry", default=None,
                    help="ctest entry name this run belongs to")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="-- <test.exe> [args]")
    args = ap.parse_args()
    if not args.command or args.command[0] != "--":
        ap.error("expected '-- <test.exe> [args...]'")
    command = args.command[1:]
    if not command:
        ap.error("no test executable given")

    live_log = live_log_path(args.qtest_log, command[0])
    budget, budget_note = _inline_budget(args)

    #Every log describes THIS run only: drop stale files so a hang is
    #never misread from an earlier run's leftovers.
    for path in (args.qtest_log, live_log, args.progress_log):
        try:
            os.remove(path)
        except OSError:
            pass
    log_dir = os.path.dirname(args.progress_log)
    if log_dir:
        os.makedirs(log_dir, exist_ok=True)

    proc = subprocess.Popen(
        command + ["-o", args.qtest_log + ",txt"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=(os.name != "nt"))
    start_mono = time.monotonic()
    saw_live = False

    def elapsed() -> float:
        return time.monotonic() - start_mono

    with open(args.progress_log, "w", encoding="utf-8", buffering=1) as out:
        out.write(f"[{clock_now()} +    0.0s] START {' '.join(command)}\n")
        #The provenance note goes to the progress log always, and to
        #the wrapper's own stdout (which ctest records in the junit
        #<system-out>) when no budget could be derived: a green run
        #stays one clean line per entry, an unpriced entry stays
        #visible. The BUDGET-EXCEEDED line below carries its own
        #budget=/actual= numbers, so the stdout note is the only
        #missing piece when the budget is None.
        out.write(f"[{clock_now()}] BUDGET {budget_note}\n")
        if budget is None:
            print(f"BUDGET {budget_note}", flush=True)
        live_offset = 0
        prev_epoch = None
        while True:
            #Tail the live sidecar: one line per finished function,
            #flushed on close, readable between the test's writes.
            try:
                with open(live_log, encoding="utf-8",
                          errors="replace") as lf:
                    lf.seek(live_offset)
                    fresh = lf.read()
                    live_offset = lf.tell()
            except OSError:
                fresh = ""
            for line in fresh.splitlines():
                parsed = parse_live_line(line)
                if parsed is None:
                    continue
                name, epoch = parsed
                dur = 0.0 if prev_epoch is None else (epoch - prev_epoch) / 1000.0
                prev_epoch = epoch
                saw_live = True
                out.write(f"[{clock_now()} +{elapsed():7.1f}s "
                          f"dur={dur:6.1f}s] finished {name}\n")

            rc = proc.poll()
            if rc is not None:
                #The -o log unlocks on process death: read the run's
                #final verdict even when no live sidecar existed.
                total = elapsed()
                out.write(f"[done rc={rc} total={total:.1f}s]\n")
                totals = totals_line(args.qtest_log)
                if totals:
                    out.write(f"[{clock_now()}] {totals}\n")
                if not saw_live:
                    last = last_result_line(args.qtest_log)
                    if last:
                        out.write(f"[{clock_now()}] last result: {last}\n")
                #Budget judgment on the finished run (the kill path
                #stays 124: that span measured the cap, not the
                #workload). A green-but-slow run turns red here; an
                #already-failing child keeps its own rc (one verdict
                #per entry).
                if budget is not None and total > budget:
                    line = (f"BUDGET-EXCEEDED {args.entry} "
                            f"(TIMEOUT_BUDGET budget={budget:.2f} "
                            f"actual={total:.2f})")
                    print(line, flush=True)
                    out.write(f"[{clock_now()}] {line}\n")
                    if rc == 0:
                        return 1
                return rc

            if args.timeout is not None and elapsed() > args.timeout:
                kill_tree(proc.pid)
                proc.wait()
                total = elapsed()
                last = last_result_line(args.qtest_log)
                out.write(f"[TIMEOUT after {total:.1f}s]\n")
                out.write(f"[{clock_now()}] last completed: "
                          f"{last or 'none'}; stuck in the function "
                          f"after it\n")
                return 124
            time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    sys.exit(main())
