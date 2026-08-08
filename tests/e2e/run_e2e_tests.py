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

#Test name suffixes that mark intentional throw-tests. Tests ending in
#these suffixes are excluded from the P3.7 hidden-throw detector: their
#exit=1 is by design (runtime throws → exit 1 = expected 1).
INTENTIONAL_THROW_SUFFIXES = (
    '_throws', '_null', '_oob', '_bounds', '_eos',
    '_missing', '_bad_mode', '_cycle',
)


def _is_intentional_throw_test(name):
    """Return True if the test name marks it as an intentional throw-test."""
    return any(name.endswith(suf) for suf in INTENTIONAL_THROW_SUFFIXES)


def main():
    ncc = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_NCC
    nvm = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_NVM

    if not os.path.isfile(MANIFEST):
        print(f"ERROR: manifest.txt not found at {MANIFEST}")
        sys.exit(1)

    passed = 0
    failed = 0
    warnings = []
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

            #Phase 8: ensure _phase8_tmp/ exists and is clean for file_stream_*/fs_struct_*/fs_object_* tests.
            if name.startswith('file_stream_') or name.startswith('fs_struct_') or name.startswith('fs_object_'):
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
            #Phase 8: file_stream_*/fs_struct_*/fs_object_* tests need CWD = tests/e2e/ for relative paths.
            run_cwd = SCRIPT_DIR if (name.startswith('file_stream_')
                                     or name.startswith('fs_struct_')
                                     or name.startswith('fs_object_')) else None
            try:
                result = subprocess.run(
                    [nvm, nmod_file],
                    capture_output=True, timeout=TIMEOUT_SEC,
                    cwd=run_cwd)
                actual = result.returncode
                stderr_text = result.stderr.decode('utf-8', errors='replace')
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
                #P3.7 hidden-throw detector: a test that "passes" only
                #because the runtime threw (exit 1) coinciding with an
                #expected=1 manifest entry. Name-suffix allowlist filters
                #out intentional throw-tests. Catches the OP_CallIntrinsic
                #class of masked bugs (commit 862d7a7).
                if (actual == 1 and 'Runtime error' in stderr_text
                        and not _is_intentional_throw_test(name)):
                    warn = (f"WARN {name} passes via throw coincidence "
                            f"(exit 1 = expected 1, but stderr has "
                            f"'Runtime error'). Intent unclear without "
                            f"test-name suffix in allowlist.")
                    print(warn)
                    warnings.append(f"  {name}: {stderr_text.splitlines()[0] if stderr_text else ''}")
            else:
                print(f"FAIL {name} (expected={expected}, actual={actual})")
                failed += 1
                errors.append(f"  {name}: expected={expected} actual={actual}")

    #Final cleanup: remove _phase8_tmp/ if it exists.
    if os.path.isdir(PHASE8_TMP):
        shutil.rmtree(PHASE8_TMP)

    print()
    print(f"Results: {passed} passed, {failed} failed")
    if warnings:
        print(f"Warnings ({len(warnings)} — possible masked bugs):")
        for w in warnings:
            print(w)
    if errors:
        print("Failures:")
        for e in errors:
            print(e)
    sys.exit(failed)


if __name__ == '__main__':
    main()
