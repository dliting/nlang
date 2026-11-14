"""Run a command while keeping the machine from sleeping.

Gate sweeps run for well over an hour, and a machine suspend mid-suite
corrupts every wall-clock assertion in it: time.monotonic() (QPC) keeps
counting across suspend, so a sleeping machine turns passing timing
budgets into failures hours "overdue". This wrapper holds a
process-scoped keep-awake request (SetThreadExecutionState) for the
lifetime of the child; when this process exits, Windows reverts to the
normal power policy. No system settings are modified.

Usage: python keep_awake_run.py <command> [args...]
"""

import ctypes
import subprocess
import sys

ES_CONTINUOUS = 0x80000000
ES_SYSTEM_REQUIRED = 0x00000001


def main():
    if len(sys.argv) < 2:
        print("usage: keep_awake_run.py <command> [args...]",
              file=sys.stderr)
        return 2
    #Thread-scoped: the request lives exactly as long as the thread
    #blocked in subprocess.call below; no cleanup path can leak it.
    ctypes.windll.kernel32.SetThreadExecutionState(
        ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
    return subprocess.call(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
