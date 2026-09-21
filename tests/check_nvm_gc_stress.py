#!/usr/bin/env python3
"""ctest guard: nvm's --gc-stress=N flag must parse from any position
(before or after the module path), and the module's exit code must pass
through unchanged under a forced collection rhythm.

Usage: python check_nvm_gc_stress.py <nvm-path> <ncc-path>
"""
import os
import subprocess
import sys
import tempfile

TIMEOUT_SEC = 60
MODULE_EXIT = 7

SOURCE = """int main() {
\t//gc-stress position guard: the module's own exit code must pass
\t//through under a forced collection rhythm.
\tint[] t = new int[4];
\treturn 7;
}
"""


def main():
    nvm = os.path.abspath(sys.argv[1])
    ncc = os.path.abspath(sys.argv[2])
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "gc_flag_pos.n")
        with open(src, "w", encoding="utf-8", newline="\n") as f:
            f.write(SOURCE)
        mod = os.path.join(tmp, "gc_flag_pos.nmod")
        r = subprocess.run([ncc, "build", src, "-o", mod],
                           capture_output=True, timeout=TIMEOUT_SEC)
        if r.returncode != 0:
            print("FAIL compile step exited %d" % r.returncode)
            print(r.stderr.decode("utf-8", "replace"))
            return 1
        for argv in ([nvm, "--gc-stress=8", mod],
                     [nvm, mod, "--gc-stress=8"]):
            r = subprocess.run(argv, capture_output=True, timeout=TIMEOUT_SEC)
            if r.returncode != MODULE_EXIT:
                print("FAIL %s exited %d, want %d"
                      % (" ".join(argv[1:]), r.returncode, MODULE_EXIT))
                print(r.stderr.decode("utf-8", "replace"))
                return 1
        print("PASS nvm --gc-stress parses before and after the module path")
        return 0


if __name__ == "__main__":
    sys.exit(main())
