#!/usr/bin/env python3
"""NLang Phase 2 end-to-end test runner.

Usage:
    conda run -n py313 python run_e2e_tests.py [ncc_path] [nvm_path]

Reads manifest.txt (name + expected_exit_code), compiles each <name>.n,
runs the .nmod, and reports results.
"""

import os
import sys
import subprocess
import shutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_NCC = os.path.join(
    SCRIPT_DIR, '..', '..', 'build', 'src', 'tools', 'ncc', 'Release', 'ncc.exe')
DEFAULT_NVM = os.path.join(
    SCRIPT_DIR, '..', '..', 'build', 'src', 'tools', 'nvm', 'Release', 'nvm.exe')
MANIFEST = os.path.join(SCRIPT_DIR, 'manifest.txt')
TIMEOUT_SEC = 30
PHASE8_TMP = os.path.join(SCRIPT_DIR, '_phase8_tmp')


def main():
    ncc = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_NCC
    nvm = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_NVM

    if not os.path.isfile(MANIFEST):
        print(f"ERROR: manifest.txt not found at {MANIFEST}")
        sys.exit(1)

    passed = 0
    failed = 0
    errors = []

    with open(MANIFEST, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            name = parts[0]
            expected = int(parts[1])

            test_file = os.path.join(SCRIPT_DIR, f"{name}.n")
            if not os.path.isfile(test_file):
                print(f"SKIP {name} (file missing)")
                continue

            #Phase 8: ensure _phase8_tmp/ exists and is clean for file_stream_* tests.
            if name.startswith('file_stream_'):
                if os.path.isdir(PHASE8_TMP):
                    shutil.rmtree(PHASE8_TMP)
                os.makedirs(PHASE8_TMP, exist_ok=True)

            # Compile
            nmod_file = os.path.join(SCRIPT_DIR, f"{name}.nmod")
            try:
                subprocess.run(
                    [ncc, 'build', test_file, '-o', nmod_file],
                    capture_output=True, timeout=TIMEOUT_SEC)
            except Exception as e:
                print(f"FAIL {name} (compile error: {e})")
                failed += 1
                errors.append(f"  {name}: compile error: {e}")
                continue

            if not os.path.isfile(nmod_file):
                # ncc may output .nmod in CWD; try looking there
                cwd_nmod = os.path.join(os.getcwd(), f"{name}.nmod")
                if os.path.isfile(cwd_nmod):
                    shutil.move(cwd_nmod, nmod_file)
                else:
                    print(f"FAIL {name} (compilation failed)")
                    failed += 1
                    errors.append(f"  {name}: compilation failed")
                    continue

            # Run
            #Phase 8: file_stream_* tests need CWD = tests/e2e/ for relative paths.
            run_cwd = SCRIPT_DIR if name.startswith('file_stream_') else None
            try:
                result = subprocess.run(
                    [nvm, nmod_file],
                    capture_output=True, timeout=TIMEOUT_SEC,
                    cwd=run_cwd)
                actual = result.returncode
            except Exception as e:
                print(f"FAIL {name} (runtime error: {e})")
                failed += 1
                errors.append(f"  {name}: runtime error: {e}")
                if os.path.isfile(nmod_file):
                    os.remove(nmod_file)
                continue

            # Clean up
            if os.path.isfile(nmod_file):
                os.remove(nmod_file)

            if actual == expected:
                print(f"PASS {name} (exit={actual})")
                passed += 1
            else:
                print(f"FAIL {name} (expected={expected}, actual={actual})")
                failed += 1
                errors.append(f"  {name}: expected={expected} actual={actual}")

    #Final cleanup: remove _phase8_tmp/ if it exists.
    if os.path.isdir(PHASE8_TMP):
        shutil.rmtree(PHASE8_TMP)

    print()
    print(f"Results: {passed} passed, {failed} failed")
    if errors:
        print("Failures:")
        for e in errors:
            print(e)
    sys.exit(failed)


if __name__ == '__main__':
    main()
