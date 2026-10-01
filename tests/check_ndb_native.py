#!/usr/bin/env python3
"""ndb --machine native search-path end-to-end check.

Builds a module importing a third-party native package, then drives a real
`ndb --machine <ncu> -I <pkg>` session with the prelude command `run`; the
program's output arrives as an `output` event. Real ncc build + real ndb run
(no mocks). Usage:
  check_ndb_native.py <ncc> <ndb> <pkg_dir> <out_dir>
"""
import os
import subprocess
import sys
import tempfile


def main() -> int:
    ncc, ndb, pkg, out_dir = sys.argv[1:5]
    os.makedirs(out_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix="ndb_native_", dir=out_dir)
    src = os.path.join(pkg, "use_mylib.n")

    ncu = os.path.join(work, "dbg.ncu")
    subprocess.run([ncc, "build", src, "-I", pkg, "-o", ncu], check=True)

    proc = subprocess.run(
        [ndb, "--machine", ncu, "-I", pkg],
        input="run\nc\n", capture_output=True, text=True, timeout=120)
    out = proc.stdout + proc.stderr
    # The program self-checks before printing, then the output event carries
    # the line; a failed native load would print an error event instead.
    assert "hello, world" in out, out
    print("ndb native machine OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
