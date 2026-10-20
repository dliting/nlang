#!/usr/bin/env python3
"""ctest guard: nvm's --carriers=N flag (K2 slice 6).

Two contracts:
  1. Carrier count never changes observable behavior for a program that
     does not spawn: --carriers=1 and --carriers=4 must emit identical
     stdout and the same pass-through exit code.
  2. Out-of-range or unparsable values are refused up front with an
     explicit diagnostic and a nonzero exit — a silently ignored knob
     could green a carrier matrix for the wrong reason (same rationale
     as the --gc-stress guard).

Usage: python check_nvm_carriers.py <nvm-path> <ncc-path>
"""
import os
import subprocess
import sys
import tempfile

TIMEOUT_SEC = 60
MODULE_EXIT = 7
BAD_VALUE_MARK = "--carriers expects 1..64"

SOURCE = """import io;
int main() {
\t//carrier parity guard: single-threaded output must not depend on how
\t//many carriers can exist.
\tint sum = 0;
\tint i = 0;
\twhile (i < 200) {
\t\tsum = sum + i;
\t\ti = i + 1;
\t}
\tio.print(sum);
\tio.print("carrier-parity");
\treturn 7;
}
"""


def run(argv):
    return subprocess.run(argv, capture_output=True, timeout=TIMEOUT_SEC)


def main():
    nvm = os.path.abspath(sys.argv[1])
    ncc = os.path.abspath(sys.argv[2])
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "carrier_parity.n")
        with open(src, "w", encoding="utf-8", newline="\n") as f:
            f.write(SOURCE)
        mod = os.path.join(tmp, "carrier_parity.ncu")
        r = run([ncc, "build", src, "-o", mod])
        if r.returncode != 0:
            print("FAIL compile step exited %d" % r.returncode)
            print(r.stderr.decode("utf-8", "replace"))
            return 1

        outputs = {}
        for count in (1, 4):
            r = run([nvm, "--carriers=%d" % count, mod])
            if r.returncode != MODULE_EXIT:
                print("FAIL --carriers=%d exited %d, want %d"
                      % (count, r.returncode, MODULE_EXIT))
                print(r.stderr.decode("utf-8", "replace"))
                return 1
            outputs[count] = r.stdout
        if outputs[1] != outputs[4]:
            print("FAIL carrier counts 1 and 4 disagree on stdout")
            print("  1: %r" % outputs[1])
            print("  4: %r" % outputs[4])
            return 1
        if not outputs[1]:
            print("FAIL the parity module produced no output")
            return 1

        for bad in ("0", "65", "abc"):
            r = run([nvm, "--carriers=%s" % bad, mod])
            stderr = r.stderr.decode("utf-8", "replace")
            if r.returncode == MODULE_EXIT:
                print("FAIL --carriers=%s ran the module instead of "
                      "refusing" % bad)
                return 1
            if BAD_VALUE_MARK not in stderr:
                print("FAIL --carriers=%s lacks the range diagnostic; "
                      "stderr: %r" % (bad, stderr[:300]))
                return 1
        print("PASS nvm --carriers parses, stays parity-neutral, and "
              "rejects out-of-range values")
        return 0


if __name__ == "__main__":
    sys.exit(main())
