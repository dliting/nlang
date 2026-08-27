#!/usr/bin/env python3
"""NLang Phase 2 end-to-end test runner.

Usage:
    conda run -n py313 python run_e2e_tests.py [ncc_path] [nvm_path]

Reads manifest.txt (name + expected_exit_code [+ optional expected stdout
substring]), compiles each <name>.n, runs the .nmod, and reports results.
For compile_error tests the optional third column is instead the substring
ncc's compile diagnostics must contain (rejection reason pinning) — works
for single-file tests and for cross-module directory tests (checked against
the stderr of the module compile that failed).
A <name>.stdin file next to the source is piped to the program's stdin.
Also reads examples_manifest.txt (when present): entries resolve against
../../examples, their .nmod and scratch artifacts live under
_examples_tmp/ (their run CWD), removed at end of run.
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
PHASE11_TMP = os.path.join(SCRIPT_DIR, '_p11_tmp')

#Shipped examples (examples/) run through the same compile+run gate.
#Entries resolve against EXAMPLES_DIR; their .nmod/stdin/scratch all
#live under EXAMPLES_TMP so the source examples/ never gains build
#artifacts (fs-writing examples get an isolated, pre-cleaned CWD).
EXAMPLES_MANIFEST = os.path.join(SCRIPT_DIR, 'examples_manifest.txt')
EXAMPLES_DIR = os.path.normpath(
    os.path.join(SCRIPT_DIR, '..', '..', 'examples'))
EXAMPLES_TMP = os.path.join(SCRIPT_DIR, '_examples_tmp')


def _manifest_configs():
    """(manifest, sources_dir, out_dir) triples to run in order."""
    configs = [(MANIFEST, SCRIPT_DIR, SCRIPT_DIR)]
    if os.path.isfile(EXAMPLES_MANIFEST):
        configs.append((EXAMPLES_MANIFEST, EXAMPLES_DIR, EXAMPLES_TMP))
    return configs

#Tests that need CWD=SCRIPT_DIR for their relative file paths, plus a
#scratch dir pre-cleaned before and removed after the run.
def _needs_script_cwd(name):
    return (name.startswith('file_stream_') or name.startswith('fs_struct_')
            or name.startswith('fs_object_') or name.startswith('stdlib_io_')
            or name.startswith('stdlib_fs_'))

#Test name suffixes that mark intentional throw-tests. Tests ending in
#these suffixes are excluded from the P3.7 hidden-throw detector: their
#exit=1 is by design (runtime throws → exit 1 = expected 1).
INTENTIONAL_THROW_SUFFIXES = (
    '_throws', '_null', '_oob', '_bounds', '_eos',
    '_missing', '_bad_mode', '_cycle',
)

#Test name prefixes that mark intentional throw-tests. Phase 9d exception
#tests are entirely about exception behavior — uncaught exceptions exit 1
#by design. The whole `exception_` family is intentional.
INTENTIONAL_THROW_PREFIXES = (
    'exception_',
)


def _is_intentional_throw_test(name):
    """Return True if the test name marks it as an intentional throw-test."""
    if any(name.endswith(suf) for suf in INTENTIONAL_THROW_SUFFIXES):
        return True
    if any(name.startswith(pfx) for pfx in INTENTIONAL_THROW_PREFIXES):
        return True
    return False


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

    for manifest_path, sources_dir, out_dir in _manifest_configs():
      with open(manifest_path, 'r', encoding='utf-8') as f:
        #Scratch dir for the examples pass: reset once before its lines
        #(each example writes distinct files, so per-entry reset is not
        #needed); the manifest pass never creates it.
        if out_dir == EXAMPLES_TMP:
            if os.path.isdir(EXAMPLES_TMP):
                shutil.rmtree(EXAMPLES_TMP)
            os.makedirs(EXAMPLES_TMP)
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            #Column 1: name; column 2: expected exit code / "compile_error";
            #optional column 3: expected stdout substring (maxsplit=2 keeps
            #spaces inside the substring; absent = no stdout assertion).
            parts = line.split(None, 2)
            if len(parts) < 2:
                continue
            name = parts[0]
            #Expected may be an int (runtime exit code) or the literal
            #"compile_error" to assert that ncc rejects the source.
            if parts[1] == "compile_error":
                expected = "compile_error"
            else:
                expected = int(parts[1])
            expected_stdout = parts[2] if len(parts) > 2 else ""

            test_file = os.path.join(sources_dir, f"{name}.n")
            test_dir = os.path.join(sources_dir, name)
            if not os.path.isfile(test_file) and not os.path.isdir(test_dir):
                if out_dir != SCRIPT_DIR:
                    print(f"FAIL {name} (file missing)")
                    failed += 1
                    errors.append(f"  {name}: file missing")
                else:
                    print(f"SKIP {name} (file missing)")
                continue

            #Directory-form (multi-module) entries only exist for the
            #tests manifest: they compile .nmod files INTO the source
            #dir, which must never happen under examples/. Multi-module
            #examples are gated by ctest (project_compile/project_run).
            if os.path.isdir(test_dir) and out_dir != SCRIPT_DIR:
                print(f"FAIL {name} (directory-form entry not allowed"
                      " in the examples manifest; gate multi-module"
                      " examples via ctest instead)")
                failed += 1
                errors.append(f"  {name}: directory-form entry not "
                              f"allowed in the examples manifest")
                continue

            #Phase 9c cross-module: multi-file tests use a directory layout.
            #Layout: <name>/ contains order.txt (module names in compile
            #order, dependency first, main last) + per-module <module>.n
            #sources. Runner compiles each in order, emits .nmod into the
            #test dir, then runs the last module's .nmod.
            if os.path.isdir(test_dir):
                order_path = os.path.join(test_dir, 'order.txt')
                if not os.path.isfile(order_path):
                    print(f"FAIL {name} (missing order.txt in test dir)")
                    failed += 1
                    errors.append(f"  {name}: missing order.txt")
                    continue
                with open(order_path, 'r', encoding='utf-8') as of:
                    modules = [m.strip() for m in of.read().split() if m.strip()]
                if not modules:
                    print(f"FAIL {name} (order.txt is empty)")
                    failed += 1
                    errors.append(f"  {name}: empty order.txt")
                    continue

                #Compile each module in order. Each gets its own .nmod
                #output into the test dir; -I points at the test dir so
                #later modules can import earlier ones.
                compile_ok = True
                reject_stderr = ''  #diagnostics of the failed compile
                for mod_name in modules:
                    src = os.path.join(test_dir, f"{mod_name}.n")
                    out = os.path.join(test_dir, f"{mod_name}.nmod")
                    try:
                        r = subprocess.run(
                            [ncc, 'build', src, '-o', out, '-I', test_dir],
                            capture_output=True, timeout=TIMEOUT_SEC)
                    except Exception as e:
                        print(f"FAIL {name} (compile error: {e})")
                        failed += 1
                        errors.append(f"  {name}: compile error: {e}")
                        compile_ok = False
                        break
                    if not os.path.isfile(out):
                        compile_ok = False
                        stderr_text = r.stderr.decode('utf-8', errors='replace') if r.stderr else ''
                        #Only record diagnostic when this is an unexpected
                        #failure; expected compile_error tests pass below and
                        #shouldn't clutter the failures summary.
                        if expected != "compile_error":
                            errors.append(f"  {name}: compile of {mod_name} failed; stderr: {stderr_text[:500]}")
                        else:
                            reject_stderr = stderr_text
                        break

                if not compile_ok:
                    if expected == "compile_error":
                        #Column 3 (rejection reason pinning) — mirrors the
                        #single-file path below against the failing module's
                        #diagnostics.
                        if expected_stdout and expected_stdout not in reject_stderr:
                            print(f"FAIL {name} (diagnostic mismatch)")
                            failed += 1
                            errors.append(f"  {name}: expected diagnostic "
                                f"{expected_stdout!r}; stderr: "
                                f"{reject_stderr[:300]}")
                            continue
                        print(f"PASS {name} (compile error as expected)")
                        passed += 1
                        #Clean partial .nmod files
                        for mn in modules:
                            p = os.path.join(test_dir, f"{mn}.nmod")
                            if os.path.isfile(p):
                                os.remove(p)
                        continue
                    print(f"FAIL {name} (compilation failed)")
                    failed += 1
                    errors.append(f"  {name}: compilation failed")
                    continue

                if expected == "compile_error":
                    print(f"FAIL {name} (expected compile_error but compiled ok)")
                    failed += 1
                    errors.append(f"  {name}: expected compile_error, compiled")
                    for mn in modules:
                        p = os.path.join(test_dir, f"{mn}.nmod")
                        if os.path.isfile(p):
                            os.remove(p)
                    continue

                #Run the last module
                main_nmod = os.path.join(test_dir, f"{modules[-1]}.nmod")
                try:
                    result = subprocess.run(
                        [nvm, main_nmod],
                        capture_output=True, timeout=TIMEOUT_SEC)
                    actual = result.returncode
                    stderr_text = result.stderr.decode('utf-8', errors='replace')
                except Exception as e:
                    print(f"FAIL {name} (runtime error: {e})")
                    failed += 1
                    errors.append(f"  {name}: runtime error: {e}")
                    for mn in modules:
                        p = os.path.join(test_dir, f"{mn}.nmod")
                        if os.path.isfile(p):
                            os.remove(p)
                    continue

                #Clean up .nmod files
                for mn in modules:
                    p = os.path.join(test_dir, f"{mn}.nmod")
                    if os.path.isfile(p):
                        os.remove(p)

                if actual == expected:
                    print(f"PASS {name} (exit={actual})")
                    passed += 1
                else:
                    print(f"FAIL {name} (expected={expected}, actual={actual})")
                    failed += 1
                    errors.append(f"  {name}: expected={expected} actual={actual}")
                    if stderr_text:
                        errors.append(f"    stderr: {stderr_text.splitlines()[0]}")
                continue

            #Pass-1 (manifest.txt) entries only. The examples pass always
            #runs with CWD = its own scratch dir, and a future example
            #whose name happens to start with one of the prefixes below
            #must not churn the phase scratch dirs mid-pass.
            needs_script_cwd = out_dir == SCRIPT_DIR and _needs_script_cwd(name)
            #Phase 8/11: ensure the scratch dirs exist and are clean for
            #file-path tests (file_stream_*/fs_*_*/stdlib_io_*/stdlib_fs_*).
            if needs_script_cwd:
                if os.path.isdir(PHASE8_TMP):
                    shutil.rmtree(PHASE8_TMP)
                os.makedirs(PHASE8_TMP, exist_ok=True)
                if os.path.isdir(PHASE11_TMP):
                    shutil.rmtree(PHASE11_TMP)
                os.makedirs(PHASE11_TMP, exist_ok=True)

            # Compile
            nmod_file = os.path.join(out_dir, f"{name}.nmod")
            try:
                compile_result = subprocess.run(
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
                elif expected == "compile_error":
                    #For compile_error tests an optional column 3 is the
                    #substring ncc's diagnostics (stderr) must contain —
                    #it pins the rejection reason, not just the failure.
                    if expected_stdout:
                        stderr_text = (compile_result.stderr.decode(
                            'utf-8', errors='replace')
                            if compile_result.stderr else '')
                        if expected_stdout not in stderr_text:
                            print(f"FAIL {name} (diagnostic mismatch)")
                            failed += 1
                            errors.append(
                                f"  {name}: expected diagnostic containing "
                                f"{expected_stdout!r}; stderr: "
                                f"{stderr_text[:300]}")
                            continue
                    print(f"PASS {name} (compile error as expected)")
                    passed += 1
                    continue
                else:
                    print(f"FAIL {name} (compilation failed)")
                    failed += 1
                    errors.append(f"  {name}: compilation failed")
                    continue

            #If we reach here, compilation succeeded. If the manifest said
            #compile_error, that's a FAIL (we expected rejection).
            if expected == "compile_error":
                print(f"FAIL {name} (expected compile_error but compiled ok)")
                failed += 1
                errors.append(f"  {name}: expected compile_error, compiled")
                if os.path.isfile(nmod_file):
                    os.remove(nmod_file)
                continue

            # Run
            #Phase 8/11: file-path tests need CWD = tests/e2e/ so their
            #relative paths (_phase8_tmp/..., _p11_tmp/...) resolve.
            run_cwd = SCRIPT_DIR if needs_script_cwd else None
            #Shipped examples always run in the scratch dir: several
            #write files next to their CWD (fs_example/, io_example.txt).
            run_cwd = out_dir if out_dir != SCRIPT_DIR else run_cwd
            #Phase 11: <name>.stdin (if present) is piped to the program —
            #io.readLine tests drive stdin through it.
            stdin_path = os.path.join(sources_dir, f"{name}.stdin")
            stdin_bytes = None
            if os.path.isfile(stdin_path):
                with open(stdin_path, 'rb') as sf:
                    stdin_bytes = sf.read()
            try:
                result = subprocess.run(
                    [nvm, nmod_file],
                    capture_output=True, timeout=TIMEOUT_SEC,
                    cwd=run_cwd, input=stdin_bytes)
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
                #Phase 11: optional stdout-substring assertion (manifest
                #column 3). Checked before declaring the pass so a wrong
                #stdout is a FAIL, not a warning.
                if expected_stdout:
                    stdout_text = result.stdout.decode('utf-8', errors='replace')
                    if expected_stdout not in stdout_text:
                        print(f"FAIL {name} (stdout missing {expected_stdout!r})")
                        failed += 1
                        errors.append(f"  {name}: stdout missing {expected_stdout!r}")
                        continue
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

    #Final cleanup: remove scratch dirs if they exist.
    if os.path.isdir(PHASE8_TMP):
        shutil.rmtree(PHASE8_TMP)
    if os.path.isdir(PHASE11_TMP):
        shutil.rmtree(PHASE11_TMP)
    if os.path.isdir(EXAMPLES_TMP):
        shutil.rmtree(EXAMPLES_TMP)

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
