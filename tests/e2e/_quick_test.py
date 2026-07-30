import subprocess, os
base = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ncc = os.path.join(base, 'build', 'src', 'tools', 'ncc', 'Release', 'ncc.exe')
nvm = os.path.join(base, 'build', 'src', 'tools', 'nvm', 'Release', 'nvm.exe')
test_dir = os.path.join(base, 'tests', 'e2e')
for name in ['func_call', 'func_recursive']:
    src = os.path.join(test_dir, f'{name}.n')
    nmod = os.path.join(test_dir, f'{name}.nmod')
    subprocess.run([ncc, 'build', src, '-o', nmod], capture_output=True)
    if os.path.isfile(nmod):
        r = subprocess.run([nvm, nmod], capture_output=True)
        print(f'{name}: exit={r.returncode}')
        os.remove(nmod)
    else:
        print(f'{name}: compilation failed')
