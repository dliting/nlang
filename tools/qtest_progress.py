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
        -- <test.exe> [extra args...]

The wrapper appends `-o <qtest-log>,txt` to the child command line. On
`--timeout` it kills the whole child tree (taskkill /T on Windows) so a
stuck debug child cannot outlive the suite, then exits 124. Without
`--timeout` the child runs to completion and the child's exit code is
passed through unchanged.
"""

import argparse
import datetime
import os
import subprocess
import sys
import time

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
        try:
            os.kill(pid, 9)
        except OSError:
            pass


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--qtest-log", required=True,
                    help="the -o file the test binary writes")
    ap.add_argument("--progress-log", required=True,
                    help="timestamped mirror written by this wrapper")
    ap.add_argument("--timeout", type=float, default=None,
                    help="kill the child tree after this many seconds")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="-- <test.exe> [args]")
    args = ap.parse_args()
    if not args.command or args.command[0] != "--":
        ap.error("expected '-- <test.exe> [args...]'")
    command = args.command[1:]
    if not command:
        ap.error("no test executable given")

    live_log = live_log_path(args.qtest_log, command[0])

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
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    start_mono = time.monotonic()
    saw_live = False

    def elapsed() -> float:
        return time.monotonic() - start_mono

    with open(args.progress_log, "w", encoding="utf-8", buffering=1) as out:
        out.write(f"[{clock_now()} +    0.0s] START {' '.join(command)}\n")
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
