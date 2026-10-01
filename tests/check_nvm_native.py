#!/usr/bin/env python3
"""nvm native search-path end-to-end check.

Builds a module that imports a third-party native package, then verifies:
  1. `nvm <ncu> -I <pkg>` finds the DLL that lives off-module.
  2. With the DLL copied beside the module, `nvm <ncu>` (no -I) finds it
     via the default module-dir search.

Real ncc build + real nvm run (no mocks). Usage:
  check_nvm_native.py <ncc> <nvm> <pkg_dir> <out_dir>
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    ncc, nvm, pkg, out_dir = sys.argv[1:5]
    os.makedirs(out_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix="nvm_native_", dir=out_dir)
    src = os.path.join(pkg, "use_mylib.n")
    dll = glob.glob(os.path.join(pkg, "nlang_mylib.*"))
    assert dll, "nlang_mylib shared library missing from package"

    # 1. -I: compile (package on -I), run with the DLL off-module.
    nmod_i = os.path.join(work, "via_i.ncu")
    subprocess.run([ncc, "build", src, "-I", pkg, "-o", nmod_i],
                   check=True)
    run_i = subprocess.run([nvm, nmod_i, "-I", pkg],
                           capture_output=True, text=True)
    assert run_i.returncode == 0, run_i.stdout + run_i.stderr
    assert "hello, world" in run_i.stdout, run_i.stdout

    # 2. Default module-dir: DLL beside the module, no -I.
    nmod_d = os.path.join(work, "via_default.ncu")
    subprocess.run([ncc, "build", src, "-I", pkg, "-o", nmod_d],
                   check=True)
    shutil.copy2(dll[0], work)
    run_d = subprocess.run([nvm, nmod_d], capture_output=True, text=True)
    assert run_d.returncode == 0, run_d.stdout + run_d.stderr
    assert "hello, world" in run_d.stdout, run_d.stdout

    print("nvm native paths OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
