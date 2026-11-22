#Unit tests for the e2e runner's per-entry timing rows and its
#table-derived kill points. Every PASS/FAIL verdict line must be followed
#immediately by that entry's TIME row, and --record-out must capture one
#raw MEASURE row per verdict (the gate-time reference recorder's only
#e2e input). The second section drives the reference-table budgets:
#--config/--table pricing, the config path guess, the uncalibrated
#band-max path, the entry-row versus coarse-row kill grading (D-3/R-17)
#and the row-less --timeout fallback. The contract is pinned end-to-end:
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
import time
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
#Sleeping stub-VM variant for the kill-point cases below: with
#NLANG_STUB_SLEEP=N in the runner environment the stub VM idles ~N-1
#seconds (ping ticks one second apart, the first echo is immediate)
#before honouring the source's exit code. MIN_CAP_S and MIN_WATCHDOG_S
#are module constants of the timing table, not per-test knobs, so a
#computed budget or watchdog cap can never sit below 60/120 s and these
#sleeps must deliberately clear them -- the cases are minute-scale by
#construction.
_NVM_STUB_SLEEP = (
    '@echo off\n'
    'setlocal\n'
    'if defined NLANG_STUB_SLEEP ping -n %NLANG_STUB_SLEEP% 127.0.0.1 '
    '>nul\n'
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


def _make_stub_tools(root, nvm_body=_NVM_STUB):
    """Stub tool tree in the build-root layout the package guard derives
    its stdlib.npkg path from (<root>/src/tools/<tool>/Release/...); the
    npkg is written last, so it is newer than both the tools and the
    repo's newest stdlib source."""
    tools = {}
    for tool, body in (('ncc', _NCC_STUB), ('nvm', nvm_body),
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


def test_gate_flag_args_accept_both_spellings():
    #A space-separated --table left among the positionals priced every
    #stub-table case from the checked-in table instead: both spellings
    #must come out of argv, not just the equal form.
    rest, config, table = runner._split_gate_flag_arg(
        ['ncc.exe', '--config', 'Debug', 'nvm.exe',
         '--table=gate.txt', 'ndb.exe'])
    assert rest == ['ncc.exe', 'nvm.exe', 'ndb.exe']
    assert config == 'Debug'
    assert table == 'gate.txt'


def test_gate_flag_args_last_occurrence_wins():
    _, config, table = runner._split_gate_flag_arg(
        ['--config=Debug', '--table=a.txt', '--config=ASan',
         '--table', 'b.txt'])
    assert config == 'ASan'
    assert table == 'b.txt'


def test_gate_flag_arg_without_value_fails_loud():
    #Dangling and empty spellings must not degrade into the checked-in
    #table default (same loud-failure rule as --record-out).
    for argv in (['--config'], ['--table'], ['--config='],
                 ['--table', '']):
        with pytest.raises(ValueError, match='--'):
            runner._split_gate_flag_arg(argv)


#Kill-point cases: one sleeping probe entry priced from a --table stub
#reference table per case. The references are chosen so the computed
#budgets and watchdog caps land where the sleeps can cross them despite
#the MIN_CAP_S/MIN_WATCHDOG_S floors (see _NVM_STUB_SLEEP). The stub
#tools sit under a /Release/ path, so --config outranks the path guess
#and a missing --config guesses Release.
_GATE_PREFIX = 'timing_gate_budget_probe'


def _gate_stub_run(request, tmp_path, *, manifest_name, table_fmt,
                   extra_args, sleep_ticks, proc_timeout, scale='1.0'):
    """One real runner pass over a single sleeping probe entry, priced
    from a stub reference table. table_fmt carries one {name}
    placeholder filled with the suffix-named entry (same concurrency
    discipline as the probes above). The ambient machine-scale variable
    is always dropped and re-set explicitly; scale=None leaves it absent
    (the uncalibrated path). Returns (stdout lines, return code, entry
    name)."""
    suffix = f'_{uuid.uuid4().hex[:8]}'
    name = f'{_GATE_PREFIX}{suffix}'
    single = os.path.join(SCRIPT_DIR, name + '.n')

    def remove_corpus():
        if os.path.exists(single):
            os.remove(single)
    remove_corpus()
    request.addfinalizer(remove_corpus)
    with open(single, 'w', encoding='utf-8', newline='\r\n') as sf:
        sf.write('0\n')

    tools = _make_stub_tools(tmp_path / 'stub_build_root',
                             nvm_body=_NVM_STUB_SLEEP)
    manifest = tmp_path / manifest_name
    manifest.write_text(f'{name} 0\n', encoding='utf-8')
    table = tmp_path / 'gate_table.txt'
    table.write_text(table_fmt.format(name=name), encoding='utf-8')
    env = dict(os.environ,
               NLANG_E2E_MANIFEST=str(manifest),
               NLANG_E2E_SCRATCH_TAG=f'{_TAG_PREFIX}{suffix}',
               NLANG_STUB_SLEEP=str(sleep_ticks))
    env.pop('NLANG_GATE_MACHINE_SCALE', None)
    #An ambient work factor would double the stub budgets from outside
    #the case; the stub rows carry ref_work=1, so the run must be pinned
    #to factor 1 to price what the table says.
    env.pop('NLANG_TEST_WORK_FACTOR', None)
    if scale is not None:
        env['NLANG_GATE_MACHINE_SCALE'] = scale
    done = subprocess.run(
        [sys.executable, RUNNER,
         tools['ncc'], tools['nvm'], tools['ndb']] + list(extra_args)
        + ['--table', str(table)],
        capture_output=True, text=True, encoding='utf-8', env=env,
        timeout=proc_timeout)
    return done.stdout.splitlines(), done.returncode, name


_GATE_TABLE_DEBUG_RELEASE = (
    '# stub gate table: the Debug config prices an entry row (ref 31 ->\n'
    '# budget 62, watchdog 310), the Release config only a coarse\n'
    '# `default` row (ref 1 -> budget 60, watchdog 120).\n'
    'e2e-Debug {name} 31.00 1 常规 e2e-serial - stub entry row\n'
    'e2e-Release default 1.00 1 常规 e2e-serial - stub default row\n')


def test_explicit_config_prices_from_its_own_rows(request, tmp_path):
    """--config beats the path guess: the tools sit under a /Release/
    path, yet with --config=Debug the kill and the budget come from the
    e2e-Debug entry row, and no guess line is printed."""
    lines, rc, name = _gate_stub_run(
        request, tmp_path, manifest_name='gate_manifest.txt',
        table_fmt=_GATE_TABLE_DEBUG_RELEASE, extra_args=['--config=Debug'],
        sleep_ticks=64, proc_timeout=120)
    assert rc == 1, lines
    fails = [ln for ln in lines if ln.startswith(f'FAIL {name}')]
    assert fails, lines
    assert '(TIMEOUT_BUDGET' in fails[0], fails[0]
    assert 'budget=62.00' in fails[0], fails[0]
    assert any(ln.startswith('TIMEOUT judging config=e2e-Debug')
               for ln in lines), lines
    assert not any('猜' in ln for ln in lines), lines


def test_missing_config_guesses_and_coarse_row_advisory_only(request,
                                                             tmp_path):
    """No --config: the /Release/ tool path guesses the config (the line
    carries 猜), and an entry resolved through the coarse `default` row
    that runs over its budget is advisory only -- PASS, rc 0, no FAIL,
    so the runner's own kill never reds a coarse-scope entry (R-17)."""
    lines, rc, name = _gate_stub_run(
        request, tmp_path, manifest_name='gate_manifest.txt',
        table_fmt=_GATE_TABLE_DEBUG_RELEASE, extra_args=[],
        sleep_ticks=64, proc_timeout=120)
    assert rc == 0, lines
    assert any('构型按路径猜为Release' in ln for ln in lines), lines
    advisories = [ln for ln in lines if ln.startswith('advisory:')]
    assert len(advisories) == 1, lines
    assert f'advisory: {name} 超预算' in advisories[0], advisories
    assert 'budget=60.00' in advisories[0], advisories
    assert any(ln.startswith(f'PASS {name}') for ln in lines), lines
    assert not any(ln.startswith(f'FAIL {name}') for ln in lines), lines


def test_default_row_entry_kills_at_watchdog_cap(request, tmp_path):
    """R-17's other half: the same coarse row keeps a hang detectable --
    sleeping past the watchdog cap (120) is a runner kill naming
    (TIMEOUT_WATCHDOG), the kill point the budget never provides for a
    coarse-scope entry."""
    lines, rc, name = _gate_stub_run(
        request, tmp_path, manifest_name='gate_manifest.txt',
        table_fmt=_GATE_TABLE_DEBUG_RELEASE, extra_args=[],
        sleep_ticks=123, proc_timeout=180)
    assert rc == 1, lines
    fails = [ln for ln in lines if ln.startswith(f'FAIL {name}')]
    assert fails, lines
    assert '(TIMEOUT_WATCHDOG' in fails[0], fails[0]
    assert 'watchdog=120.00' in fails[0], fails[0]


def test_missing_scalar_prices_band_max_and_says_uncalibrated(request,
                                                              tmp_path):
    """No NLANG_GATE_MACHINE_SCALE: the budget derives with
    SCALE_BAND_MAX (ref 10 -> 10*4*2 = 80 s) and both the startup line
    and the kill verdict say 未做本机校准, the startup line also naming
    the remedy and the 11.25 s consequence for short references (the
    old Debug-era 90 s default)."""
    lines, rc, name = _gate_stub_run(
        request, tmp_path, manifest_name='gate_manifest.txt',
        table_fmt='# stub gate table\n'
                  'e2e-Debug {name} 10.00 1 常规 e2e-serial '
                  '- stub entry row\n',
        extra_args=['--config=Debug'], sleep_ticks=83, proc_timeout=140,
        scale=None)
    assert rc == 1, lines
    assert any('未做本机校准' in ln and 'NLANG_GATE_MACHINE_SCALE' in ln
               and '11.25' in ln for ln in lines), lines
    fails = [ln for ln in lines if ln.startswith(f'FAIL {name}')]
    assert fails, lines
    assert 'budget=80.00' in fails[0], fails[0]
    assert '未做本机校准' in fails[0], fails[0]


def test_manifest_scope_row_prices_the_manifest_entries(request, tmp_path):
    """M-3 at the runner: an entry owned by a manifest named
    manifest.txt is priced by that manifest's scope row (ref 30 ->
    budget 60, so the ~62 s run is over budget), not by the wider
    `default` row (ref 100 -> budget 200, no advisory). An advisory
    naming budget=60.00 is the proof the owning scope was consulted."""
    lines, rc, name = _gate_stub_run(
        request, tmp_path, manifest_name='manifest.txt',
        table_fmt='# stub gate table\n'
                  'e2e-Debug manifest.txt 30.00 1 常规 e2e-serial '
                  '- stub manifest scope\n'
                  'e2e-Debug default 100.00 1 常规 e2e-serial '
                  '- stub default row\n',
        extra_args=['--config=Debug'], sleep_ticks=62, proc_timeout=120)
    assert rc == 0, lines
    advisories = [ln for ln in lines if ln.startswith('advisory:')]
    assert len(advisories) == 1, lines
    assert f'advisory: {name} 超预算' in advisories[0], advisories
    assert 'budget=60.00' in advisories[0], advisories


def test_advisory_suppression_is_the_cap_kill_flag(capsys, monkeypatch):
    """The 超预算 advisory's suppression for runner kills is an explicit
    flag from the pricing site (cap_killed), not a substring test on the
    verdict text: an entry name or error message carrying TIMEOUT (this
    probe is literally named TIMEOUT_*) must not silence its own
    advisory forever, while a real cap kill still suppresses it -- the
    span measured the kill cap, not the workload. In-process because the
    flag is module logic with no tool involved; the verdict/TIMEOUT row
    shape stays owned by the subprocess cases above."""
    monkeypatch.setattr(runner, 'ENTRY_ADVISORY_BUDGET', 0.0)
    started = time.monotonic() - 5.0
    runner._emit_verdict('TIMEOUT_named_probe',
                         'FAIL TIMEOUT_named_probe (file missing)',
                         started, None)
    lines = capsys.readouterr().out.splitlines()
    assert any(ln.startswith('advisory: TIMEOUT_named_probe')
               for ln in lines), lines
    assert any(ln.startswith('FAIL TIMEOUT_named_probe')
               for ln in lines), lines
    runner._emit_verdict('slow_named_probe',
                         'FAIL slow_named_probe (runtime error: boom '
                         '(TIMEOUT_WATCHDOG watchdog=120.00))',
                         started, None, cap_killed=True)
    lines = capsys.readouterr().out.splitlines()
    assert not any(ln.startswith('advisory:') for ln in lines), lines
    assert any(ln.startswith('FAIL slow_named_probe')
               for ln in lines), lines


def test_row_less_entry_falls_back_to_timeout_visibly(request, tmp_path):
    """--timeout keeps its remaining job as the row-less fallback: with
    no e2e-Release rows in the table, the entry is priced by
    --timeout=3 (a ~4 s sleep is killed) and the run prints a
    TIMEOUT-FALLBACK line so the missing row stays visible."""
    lines, rc, name = _gate_stub_run(
        request, tmp_path, manifest_name='gate_manifest.txt',
        table_fmt='# stub gate table\n'
                  'e2e-Debug default 10.00 1 常规 e2e-serial '
                  '- stub default row\n',
        extra_args=['--config=Release', '--timeout=3'],
        sleep_ticks=5, proc_timeout=45)
    assert rc == 1, lines
    assert any('TIMEOUT-FALLBACK' in ln and name in ln
               for ln in lines), lines
    fails = [ln for ln in lines if ln.startswith(f'FAIL {name}')]
    assert fails and 'timed out' in fails[0], lines
