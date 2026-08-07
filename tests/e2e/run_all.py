#!/usr/bin/env python3
import subprocess, os, sys, re

base = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ncc = os.path.join(base, 'build', 'src', 'tools', 'ncc', 'Release', 'ncc.exe')
nvm = os.path.join(base, 'build', 'src', 'tools', 'nvm', 'Release', 'nvm.exe')
test_dir = os.path.join(base, 'tests', 'e2e')

pass_count = 0
fail_count = 0
compile_fail = 0

for fname in sorted(os.listdir(test_dir)):
    if not fname.endswith('.n'):
        continue
    name = fname[:-2]
    src = os.path.join(test_dir, fname)
    nmod = os.path.join(test_dir, name + '.nmod')

    # Get expected exit code from comment
    expected = 0
    with open(src, 'r') as f:
        for line in f:
            m = re.match(r'^//\s*exit:\s*(\d+)', line)
            if m:
                expected = int(m.group(1))
                break

    # Compile
    r = subprocess.run([ncc, 'build', src, '-o', nmod], capture_output=True, text=True)
    if not os.path.isfile(nmod):
        print(f'COMPILE_FAIL: {name}')
        compile_fail += 1
        fail_count += 1
        continue

    # Run
    r = subprocess.run([nvm, nmod], capture_output=True, text=True)
    actual = r.returncode

    os.remove(nmod)

    if actual == expected:
        print(f'PASS: {name} (exit={actual})')
        pass_count += 1
    else:
        print(f'FAIL: {name} (exit={actual}, expected={expected})')
        fail_count += 1

print(f'\nResults: {pass_count} passed, {fail_count} failed ({compile_fail} compile failures)')
sys.exit(0 if fail_count == 0 else 1)
