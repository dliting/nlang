#!/usr/bin/env python3
"""Smoke test for examples/embed_host (embedding API e2e).

Usage (ctest passes all four):
    python run_embed_host.py <embed_host.exe> <ncc.exe> <stdlib.npkg> <nlang_io.dll>

Compiles a small stdlib-importing script, runs it through the embed_host
example host in a temp dir, and asserts the redirected output prefix and
the reported exit code. The temp dir carries the run-time closure: the
example exe and nlang_io.dll side by side (the native loader only
searches the exe dir) and stdlib.npkg next to the .ncu (a link-time
search dir). Paths are absolute with forward slashes (Windows subprocess
discipline).
"""

import os
import shutil
import subprocess
import sys
import tempfile

DEMO_SOURCE = (
    "import io;\n"
    "\n"
    "int main() {\n"
    "    io.print(\"hello from the embedded runtime\");\n"
    "    io.eprint(\"a diagnostic on the error channel\");\n"
    "    return 0;\n"
    "}\n"
)


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    embed_host, ncc, stdlib_npkg, nlang_io_dll = (
        os.path.abspath(a).replace('\\', '/') for a in sys.argv[1:])

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'embed_demo.n')
        with open(src, 'w', encoding='utf-8', newline='\n') as f:
            f.write(DEMO_SOURCE)
        ncu = os.path.join(tmp, 'embed_demo.ncu')
        shutil.copyfile(stdlib_npkg, os.path.join(tmp, 'stdlib.npkg'))

        compiled = subprocess.run(
            [ncc, 'build', src, '-o', ncu],
            capture_output=True, text=True, timeout=60)
        if compiled.returncode != 0:
            print('ncc failed:\n' + compiled.stdout + compiled.stderr)
            return 1

        #embed_host.exe is self-contained except for the native io module
        host_exe = os.path.join(tmp, 'embed_host.exe')
        shutil.copyfile(embed_host, host_exe)
        shutil.copyfile(nlang_io_dll, os.path.join(tmp, 'nlang_io.dll'))

        ran = subprocess.run(
            [host_exe, ncu], capture_output=True, text=True, timeout=60)

    failures = []
    if ran.returncode != 0:
        failures.append('exit code %d (want 0)' % ran.returncode)
    if '[nlang] hello from the embedded runtime' not in ran.stdout:
        failures.append('stdout missing the redirected prefix: %r' % ran.stdout)
    if 'exit code: 0' not in ran.stdout:
        failures.append('stdout missing the reported exit code: %r' % ran.stdout)
    if '[nlang-err] a diagnostic on the error channel' not in ran.stderr:
        failures.append('stderr missing the error channel prefix: %r' % ran.stderr)
    if failures:
        print('embed_host smoke FAILED:')
        for f in failures:
            print('  - ' + f)
        return 1
    print('embed_host smoke passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
