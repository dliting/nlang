#!/usr/bin/env python3
"""ctest guard: the nlang script runner's CLI and pipeline contract.

Usage: python check_nlang_tool.py <case> <nlang-path>

Cases (one ctest entry each):
  run_script   script mode end to end: a no-main script exits with the
               script's return value; an empty script exits 0 (the
               runner's D3 mapping — linked but no entry point); an
               uncaught top-level throw exits 1 with the message; a
               standard-library import resolves through the load-time
               closure (stdlib.npkg member discovery) and prints.
  c_inline     -c statements run in process; a non-ASCII payload must
               survive the wide-argv decode (byte-count exit code).
  cli_rejects  usage, script+-c conflict, missing file and compile
               errors all exit 1 with a diagnostic on stderr.
"""
import os
import subprocess
import sys

TIMEOUT_SEC = 60
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
E2E_SCRIPT = os.path.join(SCRIPT_DIR, 'e2e', 'script_basic.n')
FIXTURES = os.path.join(SCRIPT_DIR, 'fixtures', 'nlang')


def fail(msg, r=None):
    print('FAIL %s' % msg)
    if r is not None:
        print('  rc=%d' % r.returncode)
        print('  stderr=%r' % r.stderr[:400])
    return 1


def run(argv):
    return subprocess.run(argv, capture_output=True, timeout=TIMEOUT_SEC)


def case_run_script(nlang):
    r = run([nlang, E2E_SCRIPT])
    if r.returncode != 42:
        return fail('script_basic.n must exit 42', r)
    r = run([nlang, os.path.join(FIXTURES, 'empty_script.n')])
    if r.returncode != 0:
        return fail('empty script must exit 0 (D3 tool mapping)', r)
    r = run([nlang, os.path.join(FIXTURES, 'thrower.n')])
    err = r.stderr.decode('utf-8', 'replace')
    if r.returncode != 1 or 'Runtime error' not in err or 'boom' not in err:
        return fail('uncaught top-level throw surface', r)
    r = run([nlang, os.path.join(FIXTURES, 'io_hello.n')])
    out = r.stdout.decode('utf-8', 'replace')
    if r.returncode != 3 or 'hello from io' not in out:
        return fail('stdlib import through the load-time closure', r)
    if r.stderr:
        return fail('successful runs must stay quiet on stderr', r)
    return 0


def case_c_inline(nlang):
    r = run([nlang, '-c', 'int x = 6; int y = 7; return x * y;'])
    if r.returncode != 42:
        return fail('inline statements must exit 42', r)
    with open(os.path.join(FIXTURES, 'zh_inline.n'), encoding='utf-8') as f:
        payload = f.read()
    r = run([nlang, '-c', payload])
    if r.returncode != 6:
        return fail('non-ASCII -c payload byte-count exit', r)
    return 0


def case_cli_rejects(nlang):
    r = run([nlang])
    if r.returncode != 1 or b'usage' not in r.stderr:
        return fail('no-argument usage', r)
    r = run([nlang, E2E_SCRIPT, '-c', 'int x = 1;'])
    if r.returncode != 1 or b'exactly one' not in r.stderr:
        return fail('script + -c conflict', r)
    r = run([nlang, os.path.join(FIXTURES, 'missing_script.n')])
    if r.returncode != 1 or b'not found' not in r.stderr:
        return fail('missing script file', r)
    r = run([nlang, '-c', 'int x = ;'])
    err = r.stderr.decode('utf-8', 'replace')
    if r.returncode != 1 or 'Error' not in err:
        return fail('compile error diagnostics on stderr', r)
    return 0


CASES = {
    'run_script': case_run_script,
    'c_inline': case_c_inline,
    'cli_rejects': case_cli_rejects,
}


def main():
    if len(sys.argv) != 3:
        print('usage: check_nlang_tool.py <case> <nlang-path>')
        return 2
    handler = CASES.get(sys.argv[1])
    if handler is None:
        print('FAIL unknown case %r' % sys.argv[1])
        return 2
    rc = handler(os.path.abspath(sys.argv[2]))
    if rc == 0:
        print('nlang %s: OK' % sys.argv[1])
    return rc


if __name__ == '__main__':
    sys.exit(main())
