#Unit tests for the e2e runner's per-entry timing rows. Every PASS/FAIL
#verdict line must be followed immediately by that entry's TIME row, and
#--record-out must capture one raw MEASURE row per verdict (the gate-time
#reference recorder's only e2e input). The contract is pinned end-to-end:
#one real runner subprocess over stub .cmd tools laid out like a build
#root. The stale-binary guard demands tool mtimes newer than src/, and
#the package guard demands <build-root>/stdlib/stdlib.npkg newer than
#both ncc and stdlib/*.n, so the stubs are written fresh and the npkg
#last. Seconds are machine state: rows are pinned by shape and only
#bounded by >= 0, never by a magnitude.
import os
import re
import shutil
import subprocess
import sys
import uuid

import pytest

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RUNNER = os.path.join(SCRIPT_DIR, 'run_e2e_tests.py')
sys.path.insert(0, SCRIPT_DIR)

import run_e2e_tests as runner

#The stub tools stand in for the real ones: the runner invokes compiles
#as `<tool> build <src> -o <out> [-I dir]` and runs as `<tool> <ncu>`,
#so the source's first line (the wanted exit code) is copied into the
#.ncu and read back at run time. ndb is never invoked by these entries,
#but the stale guard demands the file exists.
_NCC_STUB = (
    '@echo off\n'
    'setlocal\n'
    'set /p exitcode=<"%~2"\n'
    '> "%~4" echo %exitcode%\n')
_NVM_STUB = (
    '@echo off\n'
    'setlocal\n'
    'set /p exitcode=<"%~1"\n'
    'exit /b %exitcode%\n')
_NDB_STUB = '@echo off\nexit /b 0\n'

#Corpus and scratch naming: the manifest override resolves entries and
#.ncu output against this file's directory, and the runner's scratch
#root (tests/temp) sits in the shared source tree as well — so the
#names carry a per-invocation suffix. With the machine lock removed,
#two build trees can run this ctest entry at once, and fixed names
#would collide there (FileExistsError on the corpus, clobbered .ncu
#artifacts, one run's teardown_scratch wiping the other's imports
#dir). The prefixes keep the entries' name shape untouched.
_PASS_PREFIX = 'timing_gate_pass_probe'
_FAIL_PREFIX = 'timing_gate_fail_probe'
_TAG_PREFIX = 'runner_timing_gate'

#Ceiling for the runner subprocess, kept below the ctest entry's
#TIMEOUT 60: a hung runner then dies inside subprocess.run, where the
#pytest process survives and the corpus finalizer still cleans up. A
#cap above 60 lets the ctest kill land on the pytest process itself —
#possibly mid-corpus-creation, stranding a corpus whose finalizer
#never ran (the fixture's pre-clean reclaims one such straggler).
_RUNNER_TIMEOUT_SEC = 45

#Seconds spelling accepted inside TIME/MEASURE rows (value unchecked
#beyond >= 0: wall-clock numbers are machine state, not assertions).
_SECONDS = r'\d+(?:\.\d+)?'


def _make_stub_tools(root):
    """Stub tool tree in the build-root layout the package guard derives
    its stdlib.npkg path from (<root>/src/tools/<tool>/Release/...); the
    npkg is written last, so it is newer than both the tools and the
    repo's newest stdlib source."""
    tools = {}
    for tool, body in (('ncc', _NCC_STUB), ('nvm', _NVM_STUB),
                       ('ndb', _NDB_STUB)):
        path = root / 'src' / 'tools' / tool / 'Release' / f'{tool}.cmd'
        path.parent.mkdir(parents=True)
        path.write_text(body, encoding='utf-8')
        tools[tool] = str(path)
    npkg = root / 'stdlib' / 'stdlib.npkg'
    npkg.parent.mkdir(parents=True)
    npkg.write_text('stub package', encoding='utf-8')
    return tools


@pytest.fixture
def stub_run(request, tmp_path):
    """One real runner pass over a two-entry stub manifest: one PASS
    (single-file form) and one FAIL (directory form, so the multi-module
    branch is timed too). The manifest-override pass resolves corpora
    and .ncu output against tests/e2e, so the stub corpora live there —
    per-invocation-suffixed names, created fresh, removed by the
    finalizer — while every scratch dir goes under this run's suffixed
    tag, never a concurrent run's namespace. Returns (stdout lines,
    record text, pass entry name, fail entry name)."""
    suffix = f'_{uuid.uuid4().hex[:8]}'
    pass_entry = f'{_PASS_PREFIX}{suffix}'
    fail_entry = f'{_FAIL_PREFIX}{suffix}'
    single = os.path.join(SCRIPT_DIR, pass_entry + '.n')
    fixture_dir = os.path.join(SCRIPT_DIR, fail_entry)

    def remove_corpus():
        for path in (single, fixture_dir):
            if os.path.isdir(path):
                shutil.rmtree(path, ignore_errors=True)
            elif os.path.exists(path):
                os.remove(path)
    #Reclaim a corpus stranded mid-creation by an earlier ctest-killed
    #run (its finalizer never ran) before writing this one, then
    #register the same removal for these paths.
    remove_corpus()
    request.addfinalizer(remove_corpus)

    #First line of each source = the exit code the stub VM will exit
    #with. CRLF so the stub tools' `set /p` reads clean first lines.
    with open(single, 'w', encoding='utf-8', newline='\r\n') as sf:
        sf.write('0\n')
    os.makedirs(fixture_dir, exist_ok=True)
    with open(os.path.join(fixture_dir, 'order.txt'), 'w',
              encoding='utf-8') as of:
        of.write('mod_a\nmod_b\n')
    for module in ('mod_a', 'mod_b'):
        with open(os.path.join(fixture_dir, f'{module}.n'), 'w',
                  encoding='utf-8', newline='\r\n') as sf:
            sf.write('0\n')

    tools = _make_stub_tools(tmp_path / 'stub_build_root')
    manifest = tmp_path / 'stub_manifest.txt'
    manifest.write_text(
        f'{pass_entry} 0\n'
        f'{fail_entry} 2\n', encoding='utf-8')
    out_file = tmp_path / 'out.txt'
    env = dict(os.environ,
               NLANG_E2E_MANIFEST=str(manifest),
               NLANG_E2E_SCRATCH_TAG=f'{_TAG_PREFIX}{suffix}')
    done = subprocess.run(
        [sys.executable, RUNNER,
         tools['ncc'], tools['nvm'], tools['ndb'],
         '--record-out', str(out_file)],
        capture_output=True, text=True, encoding='utf-8', env=env,
        timeout=_RUNNER_TIMEOUT_SEC)
    #One failure by construction (the directory-form entry expects exit
    #2 while the stub VM always exits 0). Anything else means the stubs
    #or the corpus layout broke — the full log is the diagnosis.
    assert done.returncode == 1, (
        f'rc={done.returncode}\n{done.stdout}\n{done.stderr}')
    record_text = (out_file.read_text(encoding='utf-8')
                   if out_file.exists() else '')
    return done.stdout.splitlines(), record_text, pass_entry, fail_entry


def _assert_time_row(lines, verdict):
    """The line right after a verdict line must be that entry's TIME
    row. Looking the verdict string up verbatim also pins the historical
    PASS/FAIL spelling byte-for-byte — the shard A/B diff greps exactly
    those lines."""
    index = lines.index(verdict)
    name = verdict.split()[1]
    match = re.fullmatch(rf'TIME {re.escape(name)} ({_SECONDS})',
                         lines[index + 1])
    assert match, f'no TIME row after {verdict!r}: {lines[index + 1]!r}'
    assert float(match.group(1)) >= 0


def test_verdicts_are_followed_by_time_and_measure_rows(stub_run):
    """The runner's timing contract: one TIME row after every PASS and
    FAIL verdict line, one MEASURE row per verdict in manifest order,
    and the verdict lines themselves unchanged."""
    lines, record_text, pass_entry, fail_entry = stub_run
    #The suffixed names stay in the entries' family shape; only the
    #suffix uniquifies them for concurrent runs.
    assert re.fullmatch(r'timing_gate_pass_probe_[0-9a-f]{8}', pass_entry)
    assert re.fullmatch(r'timing_gate_fail_probe_[0-9a-f]{8}', fail_entry)
    _assert_time_row(lines, f'PASS {pass_entry} (exit=0)')
    _assert_time_row(
        lines, f'FAIL {fail_entry} (expected=2, actual=0)')
    rows = record_text.splitlines()
    assert len(rows) == 2, f'want one MEASURE row per verdict: {rows}'
    for row, name in zip(rows, (pass_entry, fail_entry)):
        match = re.fullmatch(
            rf'MEASURE {re.escape(name)} ({_SECONDS}) form=e2e-serial',
            row)
        assert match, f'malformed MEASURE row {row!r}'
        assert float(match.group(1)) >= 0


def test_record_out_arg_splits_out_of_argv():
    rest, record = runner._split_record_out_arg(
        ['ncc.exe', '--record-out', 'measures.txt', 'nvm.exe', 'ndb.exe'])
    assert rest == ['ncc.exe', 'nvm.exe', 'ndb.exe']
    assert record == 'measures.txt'


def test_record_out_accepts_the_timeout_style_spelling():
    #'--record-out=FILE' matches how --timeout is spelled in this same
    #runner; either form must work, because a dropped flag otherwise
    #sinks silently into the positionals main() ignores.
    rest, record = runner._split_record_out_arg(
        ['ncc.exe', '--record-out=measures.txt', 'nvm.exe'])
    assert rest == ['ncc.exe', 'nvm.exe']
    assert record == 'measures.txt'


def test_record_out_arg_absent_is_a_no_op():
    rest, record = runner._split_record_out_arg(['ncc.exe', 'nvm.exe'])
    assert rest == ['ncc.exe', 'nvm.exe']
    assert record is None


def test_record_out_arg_without_path_fails_loud():
    with pytest.raises(ValueError, match='--record-out'):
        runner._split_record_out_arg(['--record-out'])


def test_record_out_arg_with_empty_path_fails_loud():
    #An empty value (--record-out= or --record-out '') would otherwise
    #land as a falsy record_out and silently disable recording — the
    #same lost-measurement face as the misspelled flag the equal-form
    #support closed. Only a real path counts.
    for argv in (['--record-out='], ['ncc.exe', '--record-out', '']):
        with pytest.raises(ValueError, match='--record-out'):
            runner._split_record_out_arg(argv)
