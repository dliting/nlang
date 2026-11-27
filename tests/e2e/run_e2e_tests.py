#!/usr/bin/env python3
"""NLang Phase 2 end-to-end test runner.

Usage:
    conda run -n py313 python run_e2e_tests.py [ncc_path] [nvm_path]
        [--timeout=SEC] [--config=Debug|ASan|Release] [--table=FILE]
        [--record-out FILE]

Reads manifest.txt (name + expected_exit_code [+ optional expected stdout
substring]), compiles each <name>.n, runs the .ncu, and reports results.
Every entry of a pass runs with that pass's fixed CWD (manifest.txt:
tests/e2e/, examples_manifest.txt: its own scratch dir), so a corpus that
writes files must address them under ../temp/, which .gitignore keeps out
of the repository — never next to the corpora.
NLANG_E2E_SCRATCH_TAG=<name> namespaces this run's scratch under
../temp/<name>/ and moves the manifest pass's run CWD into it, so several
runners can work one manifest at the same time (the shard tool gives each
shard its own tag); unset, every path and CWD is exactly as above.
--timeout=SEC now prices only entries with no gate-time table row (the
fallback kill point; absent, the fallback is DEFAULT_TIMEOUT_FALLBACK_S).
Row-backed entries take their kill from the table below, so this flag no
longer carries per-config headroom.
Every per-entry verdict line (PASS or FAIL) is followed by a
TIME <name> <seconds> line with that entry's own wall-clock span; SKIP
lines and the startup checks carry no timing. --record-out FILE
additionally writes one raw `MEASURE <name> <seconds> form=e2e-serial`
row per verdict — the gate-time reference recorder's only e2e input,
so the TIME and MEASURE rows are the plan's single e2e timing outlet.
Every entry's kill point is derived at startup from the gate-time
reference table (default tests/gate_time_reference.txt, --table=FILE
overrides; tools/gate_time_table.py holds the single caps() site every
gate consumer shares): an entry with its own row under the judging
config is killed at that row's budget and its FAIL line names
(TIMEOUT_BUDGET budget=... watchdog=...); an entry priced through a
coarse `default` or manifest-scope row is killed at that row's watchdog
cap and names (TIMEOUT_WATCHDOG watchdog=...), and finishing over that
coarse budget without being killed prints an `advisory: <name> 超预算`
line without turning the run red (a coarse row pins a scope's pace,
never one entry's own). An entry with no row at all prints a
TIMEOUT-FALLBACK line naming the missing row and runs under --timeout.
--config=CFG names the judging config (the gate legs pass it, derived
from the reference key they sweep); without it the config is guessed
from the tool path — Release or Debug (the ASan tree runs the Debug
binaries, and its leg passes --config=ASan) — and the guess is printed.
NLANG_GATE_MACHINE_SCALE (the calibrated machine scalar) scales every
budget; without it budgets price at SCALE_BAND_MAX and both the startup
note and any kill say 未做本机校准. NLANG_TEST_WORK_FACTOR scales
row-backed budgets and watchdog caps through the same formula.
For compile_error tests the optional third column is instead the substring
ncc's compile diagnostics must contain (rejection reason pinning) — works
for single-file tests and for cross-module directory tests (checked against
the stderr of the module compile that failed). Several expectations may be
listed, separated by ';'; a leading '!' negates one (the diagnostics must
NOT contain it) — the same '!' semantics as the .stderr sidecar files, and
the only way to pin that a diagnostic stays absent.
A <name>.stderr file next to the source asserts on the compile-phase
stderr of a SUCCESSFULLY compiled single-file test (warning-behavior
tests): the file's content is a substring ncc's diagnostics must
contain; a leading '!' negates (must NOT contain). This exists because
warnings are compile-time diagnostics invisible in the run's stdout —
without it, warning tests cannot express their named contract.
A <name>.stdin file next to the source is piped to the program's stdin.
A <name>.run.stderr file likewise asserts on the RUN-phase stderr of a
passing test (io.eprint's console target) — same substring and leading-'!'
negation semantics as the compile-phase .stderr file.
Also reads examples_manifest.txt (when present): entries resolve against
../../examples, their .ncu and scratch artifacts live under
../temp/examples/ (their run CWD), removed at end of run.
Reads env_gates.txt (when present): `name <capability...>` rows gate an
entry behind a machine capability (icmp: OS loopback echo answers; dns:
reserved-TLD NXDOMAIN honesty). When the out-of-band OS oracle says the
capability is absent (VPN filter drivers, DNS hijacks), the entry is
skipped with a printed SKIP line — counted, never silently green.
"""

import os
import sys
import glob
import socket
import subprocess
import shutil
import shlex
import time
import re
from collections import namedtuple

#The startup notice, kill notes and advisory lines are Chinese (project
#docs language) and are parsed by the gate guards and junit post-
#processing, so their encoding must not follow the leg environment; the
#consumers decode UTF-8 (the pytest harness with encoding='utf-8', ctest
#into junit XML). Pin the text streams — measured under the registered ctest
#environment on a zh-CN machine, the child otherwise encodes them cp936,
#which crashed the parent's subprocess reader thread (a UnicodeDecodeError
#surfacing as stdout=None despite capture_output=True).
sys.stdout.reconfigure(encoding='utf-8')
sys.stderr.reconfigure(encoding='utf-8')

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, '..', '..'))
DEFAULT_NCC = os.path.join(
    SCRIPT_DIR, '..', '..', 'build', 'src', 'tools', 'ncc', 'Release', 'ncc.exe')
DEFAULT_NVM = os.path.join(
    SCRIPT_DIR, '..', '..', 'build', 'src', 'tools', 'nvm', 'Release', 'nvm.exe')
MANIFEST = os.path.join(SCRIPT_DIR, 'manifest.txt')
#Tensor-backend pinning for the run phase. The product default is `auto`
#(spec D3a — use an installed device backend, fall back to cpu), so a
#machine with a GPU would otherwise move this whole suite's float paths
#onto the device and make the gate's results hardware-dependent. The
#suite therefore pins one backend explicitly; NLANG_E2E_TENSOR_BACKEND
#selects which run this is (cpu / mirror / cuda — the double-run and the
#conditional cuda tree). A fixture's own `.flags` still wins: the nvm CLI
#overrides the inherited environment.
E2E_TENSOR_BACKEND = os.environ.get('NLANG_E2E_TENSOR_BACKEND', 'cpu')
RUN_ENV = dict(os.environ)
RUN_ENV['NLANG_TENSOR_BACKEND'] = E2E_TENSOR_BACKEND
#Skip sentinel (multi-GPU spec's cross-platform ruling): a fixture that
#cannot run its case on this machine — a device census too small, a
#capability absent — prints the reason to stderr and exits this code;
#the manifest still expects the ordinary success (0). The runner turns
#it into a visible SKIP with the reason (the case ran nothing, but the
#compile ran and the contract held); a sentinel without a reason is a
#FAIL — a silent skip could mask a real breakage.
E2E_SKIP_EXIT = 90
#Kill cap for an entry with no gate-time table row; --timeout=SEC
#overrides it per invocation (see the module docstring). Row-backed
#entries take their kill from the table instead: the old flat 30 s cap
#and its Debug x3 config branch are reference rows now, and 90 s keeps
#the ceiling the x3 branch gave row-less entries under Debug.
DEFAULT_TIMEOUT_FALLBACK_S = 90
#Coarse-scope budget for the 超预算 advisory of the entry being run
#(set by main() before each entry): _emit_verdict is the single verdict
#outlet and reads it there, so no branch has to plumb it. None means
#this entry's verdicts carry no advisory (priced by its own row, or no
#row at all).
ENTRY_ADVISORY_BUDGET = None
#Seconds spelling for the per-entry TIME/MEASURE rows: fixed decimals,
#so the rows stay byte-parseable for the gate-time reference recorder.
_TIMING_SECONDS_FMT = '{:.2f}'
#Root of every scratch dir the runner owns: tests/temp/, which .gitignore
#keeps out of the repository.
SCRATCH_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, '..', 'temp'))
#Scratch namespace for concurrent runs, selected by
#NLANG_E2E_SCRATCH_TAG (the shard tool sets one tag per shard). Concurrent
#runs share the manifest, the sources and the .ncu output dir, but they
#cannot share scratch: every run wipes its scratch per entry and again at
#teardown, and wiping a file a sibling process still holds open raises on
#Windows. The fs-writing corpora spell their scratch the literal relative
#path ../temp/fs/..., resolved against the run CWD, so a tag nests its
#whole scratch set one level down and moves the run CWD with it — the same
#literal then resolves inside the tag:
#    tests/temp/<tag>/cwd/../temp/fs == tests/temp/<tag>/temp/fs
#Renaming the scratch dir instead would mean editing every corpus that
#writes files. Unset, every path keeps its historical spelling and the
#manifest pass keeps running in tests/e2e.
_SCRATCH_DIRS = namedtuple('ScratchDirs', ('ns', 'fs', 'imports',
                                           'examples', 'run_cwd'))
#One directory name, nothing else: separators, '.'/'..' and blank space are
#all out, so a tag cannot escape tests/temp/ or name an ambiguous dir.
_SCRATCH_TAG_RE = re.compile(r'[A-Za-z0-9][A-Za-z0-9_-]*\Z')


def _validated_scratch_tag(tag):
    """Accept only a plain directory name as a scratch tag: the tag comes
    from an environment variable and the layout relies on it naming one
    directory under tests/temp/, nothing else. A name, not a path, so the
    allowed characters are the ones that can never mean anything else."""
    if not tag:
        return ''
    if not _SCRATCH_TAG_RE.match(tag):
        raise ValueError(
            f'NLANG_E2E_SCRATCH_TAG must be a single directory name, '
            f'got {tag!r}')
    return tag


def scratch_paths(tag):
    """(ns, fs, imports, examples, run_cwd) scratch dirs for one scratch
    tag; untagged keeps the historical dirs and no private run CWD."""
    tag = _validated_scratch_tag(tag)
    if not tag:
        return _SCRATCH_DIRS(
            ns=None,
            fs=os.path.join(SCRATCH_ROOT, 'fs'),
            imports=os.path.join(SCRATCH_ROOT, 'imports'),
            examples=os.path.join(SCRATCH_ROOT, 'examples'),
            run_cwd=None)
    ns = os.path.join(SCRATCH_ROOT, tag)
    return _SCRATCH_DIRS(
        ns=ns,
        #<ns>/temp/fs, the sibling ../temp/fs resolves to from <ns>/cwd.
        fs=os.path.join(ns, 'temp', 'fs'),
        imports=os.path.join(ns, 'imports'),
        examples=os.path.join(ns, 'examples'),
        run_cwd=os.path.join(ns, 'cwd'))


SCRATCH_TAG = _validated_scratch_tag(
    os.environ.get('NLANG_E2E_SCRATCH_TAG', ''))
_SCRATCH = scratch_paths(SCRATCH_TAG)
#Namespace root a tagged run owns outright, None when untagged.
SCRATCH_NS = _SCRATCH.ns
#Scratch for corpora that write files by relative path (../temp/fs/...).
FS_SCRATCH = _SCRATCH.fs
#Per-test artifact dir for multi-module imports (see the directory-form
#compile block): holds ONLY the earlier modules' .ncu files.
IMPORTS_TMP = _SCRATCH.imports
#Run CWD of a tagged manifest pass, None when untagged (see run_dir_for).
RUN_CWD = _SCRATCH.run_cwd

#Shipped examples (examples/) run through the same compile+run gate.
#Entries resolve against EXAMPLES_DIR; their .ncu/stdin/scratch all
#live under EXAMPLES_TMP so the source examples/ never gains build
#artifacts (fs-writing examples get an isolated, pre-cleaned CWD).
EXAMPLES_MANIFEST = os.path.join(SCRIPT_DIR, 'examples_manifest.txt')
EXAMPLES_DIR = os.path.normpath(
    os.path.join(SCRIPT_DIR, '..', '..', 'examples'))
EXAMPLES_TMP = _SCRATCH.examples


def reset_scratch_dir(path):
    """Wipe and recreate one scratch dir: an entry must not meet another
    entry's leftovers, and the dir has to exist for relative writes. The
    wipe is scoped to the path handed in, so a concurrent run under
    another tag — and any file it still holds open — stays untouched."""
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path, exist_ok=True)


def prepare_scratch():
    """Create the scratch this run owns before its first entry: the fs
    scratch corpora write into, plus the private run CWD of a tagged run
    (an untagged run works in tests/e2e, which is already there)."""
    reset_scratch_dir(FS_SCRATCH)
    if RUN_CWD:
        reset_scratch_dir(RUN_CWD)


def teardown_scratch():
    """Drop this run's scratch: a stale file left behind would be the next
    run's first file-path corpus reading someone else's output. A tagged
    run owns a whole namespace root, so one removal clears everything under
    it without reaching into a sibling tag's files."""
    if SCRATCH_NS:
        if os.path.isdir(SCRATCH_NS):
            shutil.rmtree(SCRATCH_NS)
        return
    for scratch in (FS_SCRATCH, IMPORTS_TMP, EXAMPLES_TMP):
        if os.path.isdir(scratch):
            shutil.rmtree(scratch)


def run_dir_for(out_dir):
    """CWD the program runs in, so its relative paths resolve where this
    run owns them. A tagged manifest pass runs in its private dir; every
    other pass keeps running in its out_dir, as before (tests/e2e for the
    manifest pass, the examples scratch for the examples pass)."""
    if out_dir == SCRIPT_DIR and RUN_CWD:
        return RUN_CWD
    return out_dir


def _newest_file(paths):
    """(mtime, relpath) of the newest existing path, (0.0, '') if none."""
    best_mtime, best_name = 0.0, ''
    for path in paths:
        mtime = os.path.getmtime(path)
        if mtime > best_mtime:
            best_mtime = mtime
            best_name = os.path.relpath(path, REPO_ROOT)
    return best_mtime, best_name


def _display_path(path):
    """Path spelling for messages: repo-relative when possible. On
    Windows os.path.relpath raises when path and the repo sit on
    different drives (no relative form exists) — fall back to the
    absolute path instead of crashing the guard itself."""
    try:
        return os.path.relpath(path, REPO_ROOT)
    except ValueError:
        return os.path.normpath(path)


def _fail_on_stale_tools(ncc, nvm, ndb):
    """Refuse to run against artifacts older than their real inputs.

    A targeted build refreshes only its own targets, so a changed VM
    source can leave one of ncc/nvm/ndb stale while its siblings look
    fresh; dbg_/dbgm tests then fail on runtime lookups in ways that
    read as test-content bugs (the 2026-10-31 "Thread class not
    registered" session: ndb predated the suffix-lookup commit). The
    check over-approximates by design — ANY newer file under src/ or
    include/ demands a rebuild of the tools; a false alarm costs one
    build, a miss costs a misdiagnosed gate.

    Each artifact is compared against its own inputs only. stdlib/*.n
    is not a tool input (the tools compile it, they do not embed it),
    so it belongs to the package check below — keeping it in the tool
    set pressured a no-op tool rebuild on every stdlib edit and tempted
    the timestamp-forging `touch` that once let a pre-merge nvm.exe
    pass this guard for a full task (2026-11-08).
    The native modules (src/native, dlopen'd at runtime) and the IDE
    sources (src/tools/nide, built only behind NLANG_BUILD_IDE) are
    excluded the same way: no tool links either, so their edits can
    never advance the tools' mtime and the alarm they raise is one no
    tool rebuild can ever clear. A stale native .so is caught by its
    own check; a stale IDE binary belongs to the IDE tree's gate.
    """
    def _is_non_tool_source(path):
        parts = os.path.relpath(path, REPO_ROOT).split(os.sep)
        if len(parts) < 2 or parts[0] != 'src':
            return False
        if parts[1] == 'native':
            return True
        return len(parts) >= 3 and parts[1] == 'tools' \
            and parts[2] == 'nide'

    newest_src, newest_name = _newest_file(
        path
        for pattern in ('src/**/*.cpp', 'src/**/*.hpp', 'src/**/*.h',
                        'include/**/*.h', 'include/**/*.hpp')
        for path in glob.glob(os.path.join(REPO_ROOT, pattern),
                              recursive=True)
        if not _is_non_tool_source(path))
    for tool in (ncc, nvm, ndb):
        if not os.path.isfile(tool):
            print(f"ERROR: tool not found: {tool}")
            sys.exit(2)
        if os.path.getmtime(tool) < newest_src:
            print(f"ERROR: {_display_path(tool)} is older "
                  f"than {newest_name} - rebuild the tools before "
                  f"running the suite (stale-binary results are "
                  f"meaningless)")
            sys.exit(2)
    #Load-time closure hazard: any .npkg in the tests directory is on
    #the module-local search path and shadows the system stdlib packages
    #for every module run from here (an experiment leftover here once
    #made manual runs pass while the harness failed).
    for pkg in glob.glob(os.path.join(SCRIPT_DIR, '*.npkg')):
        print(f"ERROR: {pkg} shadows the system stdlib for modules run "
              f"from tests/e2e; remove it")
        sys.exit(2)
    #Stale-package hazard: the stdlib packages are compiled by ncc from
    #the stdlib sources (the nlang_stdlib_package step), so each must be
    #at least as new as BOTH of its inputs — a package older than the
    #newest source misses the edit (the 2026-10-31 redesign ran green
    #against a pre-flip npkg for a full task), and a package older than
    #ncc was built by a superseded compiler. The tree's generated
    #projects name the package set (<build>/stdlib/<domain>.nproj, one
    #per domain): a project without its package means the packing step
    #has not run, and every import of that domain would fail at load
    #time. Derived from the ncc path (<build>/src/tools/ncc/<cfg>/
    #ncc.exe); skipped when no project sits at that layout (an installed
    #tool tree ships its packages beside the binaries, not under a build
    #root).
    build_root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.dirname(ncc)))))
    projects = sorted(glob.glob(os.path.join(
        build_root, 'stdlib', '*.nproj')))
    if projects:
        #The packing step's own source globs: the top-level units and the
        #web package's dotted sub-module arm.
        newest_n, newest_n_name = _newest_file(
            [path
             for pattern in ('stdlib/*.n', 'stdlib/web/*.n')
             for path in glob.glob(os.path.join(REPO_ROOT, pattern))])
        for project in projects:
            npkg = os.path.splitext(project)[0] + '.npkg'
            if not os.path.isfile(npkg):
                print(f"ERROR: {_display_path(npkg)} is missing - build "
                      f"the nlang_stdlib_package target before running "
                      f"the suite (a missing package fails every import "
                      f"of its domain at load time)")
                sys.exit(2)
            for bound, bound_name in (
                    (os.path.getmtime(ncc), _display_path(ncc)),
                    (newest_n, newest_n_name)):
                if os.path.getmtime(npkg) < bound:
                    print(f"ERROR: {_display_path(npkg)} is "
                          f"older than {bound_name} - rebuild the "
                          f"nlang_stdlib_package target before running the "
                          f"suite (stale-package results are meaningless)")
                    sys.exit(2)


def _manifest_configs():
    """(manifest, sources_dir, out_dir) triples to run in order.
    NLANG_E2E_MANIFEST=<path> selects ONLY that manifest — the tiered
    gate's subset leg runs a family file in place of the full tour (the
    orphan-sidecar check is meaningless for a subset and is skipped,
    see main)."""
    override = os.environ.get('NLANG_E2E_MANIFEST')
    if override:
        return [(os.path.abspath(override), SCRIPT_DIR, SCRIPT_DIR)]
    configs = [(MANIFEST, SCRIPT_DIR, SCRIPT_DIR)]
    if os.path.isfile(EXAMPLES_MANIFEST):
        configs.append((EXAMPLES_MANIFEST, EXAMPLES_DIR, EXAMPLES_TMP))
    return configs

#Environment gating: an entry whose live-success premise needs a
#machine capability that ambient software can revoke (VPN filter
#drivers swallowing ICMP, DNS hijacks answering reserved TLDs) is
#skipped, visibly, when an out-of-band OS oracle says the capability
#is absent. Gates live in env_gates.txt (name -> capability keys), a
#bypass file rather than a manifest column: the stdout column is free
#text, and manifest-verbatim consumers (tier coverage, shard tool,
#subset derivation) stay untouched. Pure-contract assertions are never
#gated — the oracle only admits "the machine cannot do this at all".
ENV_GATES_FILE = os.path.join(SCRIPT_DIR, 'env_gates.txt')

def _probe_icmp():
    """True when the OS itself gets loopback ICMP echo answers
    (locale-independent exit code; 2 attempts so one lost echo is
    noise, not an outage verdict)."""
    cmd = (['ping', '-n', '2', '-w', '800', '127.0.0.1']
           if os.name == 'nt'
           else ['ping', '-c', '2', '-W', '1', '127.0.0.1'])
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0

def _probe_dns():
    """True when the resolver honestly fails a reserved TLD
    (RFC 2606: .invalid is guaranteed NXDOMAIN on any compliant
    resolver; fake-ip DNS hijacks answer it, voiding the premise of
    resolver-phrase tests)."""
    try:
        socket.getaddrinfo('nlang-no-such-host.invalid', None)
        return False
    except socket.gaierror:
        return True

ENV_PROBES = {'icmp': _probe_icmp, 'dns': _probe_dns}

def parse_env_gates(path):
    """name -> set of capability keys. Unknown keys and keyless rows
    fail loud: a silent no-op gate would run environment-dependent
    entries unguarded."""
    gates = {}
    if not os.path.isfile(path):
        return gates
    with open(path, encoding='utf-8') as gf:
        for line in gf:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            name, keys = parts[0], parts[1:]
            if not keys:
                raise ValueError(f"env_gates.txt: {name}: no capability key")
            unknown = [k for k in keys if k not in ENV_PROBES]
            if unknown:
                raise ValueError(
                    f"env_gates.txt: {name}: unknown capability "
                    f"{','.join(unknown)}")
            gates[name] = set(keys)
    return gates

def probe_env(keys_needed):
    """capability -> availability for the union of keys the gates
    reference; probes run at most once per invocation."""
    return {key: ENV_PROBES[key]() for key in sorted(keys_needed)}

def env_skip_reason(name, gates, env_state):
    """Skip reason for a gated entry whose capabilities are absent,
    else None. env_state is injected so tests never subprocess."""
    keys = gates.get(name)
    if not keys:
        return None
    missing = [key for key in sorted(keys) if not env_state.get(key)]
    return f"env {','.join(missing)} unavailable" if missing else None

def _orphan_run_stderr_sidecars(known_names):
    """Names of <name>.run.stderr files whose <name> is no manifest
    entry. The runner asserts run-phase stderr only when the sidecar
    exists, so a rename that forgets the sidecar silently drops the
    assertion — an orphan is a suite defect, not dead weight."""
    orphans = []
    for path in sorted(glob.glob(os.path.join(SCRIPT_DIR, '*.run.stderr'))):
        stem = os.path.basename(path)[:-len('.run.stderr')]
        if stem not in known_names:
            orphans.append(stem)
    return orphans

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


def _is_debugger_run(name):
    """dbg_ runs under interactive ndb, dbgm_ under ndb --machine; both
    always need their .stdin command script (ndb stops immediately)."""
    return name.startswith('dbg_') or name.startswith('dbgm_')


def _runner_argv(name, ndb, nvm):
    """Tool argv for one manifest entry. dbgm_ speaks the machine line
    protocol (--machine); dbg_ the interactive CLI; everything else is
    plain nvm."""
    if name.startswith('dbgm_'):
        return [ndb, '--machine']
    if name.startswith('dbg_'):
        return [ndb]
    return [nvm]


def _run_env(name, sources_dir):
    """The run-phase environment for one entry: the suite's pinned
    tensor backend plus the entry's own `.env` sidecar (KEY=VALUE lines,
    '#' comments) — the only way a fixture can pin environment-driven
    behavior such as NLANG_TENSOR_BACKEND's unknown-name diagnostic,
    which the nvm CLI rejects before the module ever loads."""
    env = dict(RUN_ENV)
    path = os.path.join(sources_dir, f"{name}.env")
    if os.path.isfile(path):
        with open(path, encoding='utf-8') as ef:
            for line in ef:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                key, _, value = line.partition('=')
                env[key.strip()] = value.strip()
    return env


#Exit-code width the OS actually carries: POSIX wait(2) reports 8 bits
#(WEXITSTATUS), so a program exiting 999 surfaces as 231 and the
#suite's composed full-width codes cannot cross that boundary on a
#POSIX host (first seen on the acceptance box: seven corpus rows with
#expected codes over 255 failed on the exact low-byte truncation while
#passing on Windows). The comparison runs in OS-visible width: on
#POSIX both sides are masked to that width, which keeps every existing
#corpus row meaningful on both hosts (rows at or under 255 compare
#identically either way). Windows carries the full code and keeps the
#unmasked comparison. Crash verdicts ride signals (negative return
#codes) and bypass the mask entirely.
def _exit_width(code):
    if os.name != 'nt' and code is not None and code >= 0:
        return code & 0xFF
    return code


def _skip_sentinel(actual, expected, stderr_text):
    """Skip-sentinel verdict for one finished run: None when the entry
    is not on the sentinel path, else the reason string ('' when the
    sentinel arrived without naming one)."""
    if actual != E2E_SKIP_EXIT or expected == E2E_SKIP_EXIT:
        return None
    return next((ln.strip() for ln in stderr_text.splitlines()
                 if ln.strip()), '')


def _matchable_stdout(name, raw_stdout):
    """Decode a run's stdout for manifest substring matching. Machine-mode
    (dbgm_) events are tab-joined; separator tabs fold to spaces so
    manifest substrings stay space-separated. Protocol fields never carry
    raw tabs (EncodeField escapes them to literal \\t), so folding only
    ever touches framework separators — no false hits."""
    text = raw_stdout.decode('utf-8', errors='replace')
    if name.startswith('dbgm_'):
        text = text.replace('\t', ' ')
    return text


def _matchable_stderr(raw_stderr):
    """Run-phase stderr decoded for comparison. The MSVC CRT opens
    stderr in text mode on Windows, so every newline the VM prints
    lands as \\r\\n when captured; sidecars are written with plain \\n
    (the platform-neutral spelling — multi-line .run.stderr pins like
    the v0.12.4 cause-chain reports need it). Fold \\r\\n before any
    substring comparison."""
    text = raw_stderr.decode('utf-8', errors='replace') if raw_stderr else ''
    return text.replace('\r\n', '\n')


def _rejection_expectations(expected):
    """Manifest column 3 for compile_error rows, parsed into
    (negate, substring) pairs: expectations separated by ';', each
    optionally negated by a leading '!' (the diagnostics must NOT
    contain it) — the same '!' semantics as the .stderr sidecar files.
    Negation is how a rejection pins an ABSENT diagnostic (e.g. the
    package note on a name with no binding rows)."""
    out = []
    for part in expected.split(';'):
        part = part.strip()
        if not part:
            continue
        negate = part.startswith('!')
        want = part.lstrip('!').strip() if negate else part
        out.append((negate, want))
    return out


def _rejection_violation(expected, stderr):
    """First column-3 expectation the reject stderr violates, as a
    (negate, substring) pair; None when every expectation holds."""
    for negate, want in _rejection_expectations(expected):
        if (want in stderr) == negate:
            return negate, want
    return None


def _describe_run_error(exc):
    """Human-readable form of a child-run exception. A negative
    TimeoutExpired timeout is not slowness: time.monotonic() (QPC) keeps
    counting while the machine is suspended, so a sleep mid-run leaves
    the deadline far in the past when the pipes finally close. Name the
    wall-clock jump instead of printing a baffling negative seconds
    count."""
    if (isinstance(exc, subprocess.TimeoutExpired)
            and exc.timeout is not None and exc.timeout < 0):
        return "wall-clock jump during run (machine suspend?)"
    return str(exc)


def _split_timeout_arg(argv):
    """Pull a '--timeout=SEC' override out of argv (the positional tool
    paths stay untouched). The last occurrence wins; a malformed number
    propagates as a plain ValueError — this flag is set by gate scripts,
    not typed by hand."""
    timeout = None
    rest = []
    for a in argv:
        if a.startswith('--timeout='):
            timeout = int(a.split('=', 1)[1])
        else:
            rest.append(a)
    return rest, timeout


def _split_record_out_arg(argv):
    """Pull a '--record-out <file>' (or '--record-out=<file>', the
    --timeout= spelling of this same file) raw-measure sink out of argv
    (the positional tool paths stay untouched), mirroring
    _split_timeout_arg: the flag is set by the recording scripts, not
    typed by hand, so a flag with no usable path after it — dangling or
    empty — propagates as a plain ValueError rather than being silently
    left among the positionals or disabling recording as a falsy path.
    The last occurrence wins."""
    record_out = None
    rest = []
    index = 0
    while index < len(argv):
        arg = argv[index]
        value = None
        if arg == '--record-out':
            if index + 1 < len(argv):
                value = argv[index + 1]
                index += 2
            else:
                index += 1  #dangling flag: value stays None
        elif arg.startswith('--record-out='):
            value = arg.split('=', 1)[1]
            index += 1
        else:
            rest.append(arg)
            index += 1
            continue
        if not value:
            raise ValueError('--record-out requires a file path')
        record_out = value
    return rest, record_out


def _split_gate_flag_arg(argv):
    """Pull the last '--config CFG'/'--config=CFG' and
    '--table FILE'/'--table=FILE' gate flags out of argv, mirroring
    _split_record_out_arg: a space-separated spelling left among the
    positionals silently prices the run from the checked-in table
    instead of the named file (measured with the stub-table cases), so
    both spellings must be honoured here. The last occurrence of each
    flag wins; a dangling or empty value propagates as a plain
    ValueError rather than falling back, and an unknown --config value
    fails in _resolve_judging_config."""
    flag_names = {'--config': 'config', '--table': 'table'}
    values = {'config': None, 'table': None}
    rest = []
    index = 0
    while index < len(argv):
        arg = argv[index]
        head, sep, inline = arg.partition('=')
        if arg in flag_names:
            name = flag_names[arg]
            if index + 1 < len(argv):
                value = argv[index + 1]
                index += 2
            else:
                value = None  # dangling flag: caught below
                index += 1
        elif sep and head in flag_names:
            name = flag_names[head]
            value = inline
            index += 1
        else:
            rest.append(arg)
            index += 1
            continue
        if not value:
            raise ValueError('%s requires a value' % arg)
        values[name] = value
    return rest, values['config'], values['table']


def _load_gate_time_table():
    """The leaf timing-table module (tools/gate_time_table.py) loaded by
    path — the same importlib trick tools/qtest_progress.py uses: one
    caps()/resolve site is shared by every consumer, and loading the
    leaf keeps the runner free of the checker package's sibling
    imports."""
    import importlib.util
    path = os.path.join(REPO_ROOT, 'tools', 'gate_time_table.py')
    spec = importlib.util.spec_from_file_location('gate_time_table', path)
    #An absent tool file is missing input, not a silent fallback.
    if spec is None or spec.loader is None:
        raise RuntimeError(f'cannot load {path}')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _resolve_judging_config(config, ncc):
    """Which config key's rows price this run: e2e-<--config> when the
    leg named one, else the two-way tool-path guess the retired config
    branch used (a Release build tree, versus Debug — an ASan sweep
    runs the Debug binaries but its leg passes --config=ASan). Returns
    (key, guessed segment or None); an unknown --config raises ValueError
    like the other gate flags."""
    if config:
        if config not in ('Debug', 'ASan', 'Release'):
            raise ValueError('--config must be Debug, ASan or Release, '
                             'got %r' % (config,))
        return 'e2e-' + config, None
    guess = 'Release' if '/Release/' in ncc.replace(os.sep, '/') else 'Debug'
    return 'e2e-' + guess, guess


def _price_e2e_entry(table_mod, rows, config_key, name, manifest_scope,
                     work_factor, scalar, uncalibrated):
    """(kill_s, kill_note, advisory_budget_s) for one entry, graded by
    the row that prices it: the entry's own row -> kill at the budget,
    kill note naming (TIMEOUT_BUDGET ...), no advisory (the kill already
    carries the cap); a coarse `default` or manifest-scope row -> kill
    at the watchdog cap, note naming (TIMEOUT_WATCHDOG ...), and an
    advisory when the entry finishes over the budget; no row ->
    (None, '', None) and the caller applies the --timeout fallback
    visibly. An uncalibrated run labels its kill notes so the log says
    why the budget is machine-wide."""
    row = table_mod.resolve_scoped_row(rows, config_key, name,
                                       manifest_scope)
    if row is None:
        return None, '', None
    budget, watchdog = table_mod.caps_values(row.ref_s, work_factor,
                                             scalar)
    if row.entry == name:
        kill_s, advisory = budget, None
        note = f'(TIMEOUT_BUDGET budget={budget:.2f} ' \
               f'watchdog={watchdog:.2f})'
    else:
        kill_s, advisory = watchdog, budget
        note = f'(TIMEOUT_WATCHDOG watchdog={watchdog:.2f})'
    if uncalibrated:
        note += ' 未做本机校准'
    return kill_s, f' {note}', advisory


def _is_cap_kill(exc):
    """Whether the exception is the runner's own kill — a TimeoutExpired
    at the derived kill point. _kill_note attaches the cap-provenance
    note on exactly this predicate, and the pricing sites pass it to
    _emit_verdict as cap_killed: whether a verdict is a cap kill is a
    fact of the call site, never text to be substring-matched (entry
    names and compiler error texts can contain TIMEOUT)."""
    return isinstance(exc, subprocess.TimeoutExpired)


def _kill_note(exc, note):
    """Only the runner's own kill — a TimeoutExpired at the derived kill
    point — carries the cap-provenance note: any other exception text
    already describes the entry's own failure, not a cap."""
    return note if note and _is_cap_kill(exc) else ''


def _emit_verdict(name, verdict, entry_started, record_file,
                  cap_killed=False):
    """Single outlet for one per-entry verdict: the coarse-scope budget
    advisory when this entry finished over it (a runner kill at the
    derived cap — cap_killed, the same predicate _kill_note uses for the
    cap note — measured the cap, not the workload, so it needs no second
    line), then the verdict line verbatim (its consumers — shard A/B
    diff, gate logs — grep the PASS/FAIL glyphs), then the entry's TIME
    row, and when --record-out is active the matching MEASURE row with
    the same seconds reading. No branch may print a verdict directly:
    that keeps the TIME rows adjacent to every verdict without repeating
    the timing logic across the branches."""
    span = time.monotonic() - entry_started
    if (ENTRY_ADVISORY_BUDGET is not None and span > ENTRY_ADVISORY_BUDGET
            and not cap_killed):
        print(f"advisory: {name} 超预算 "
              f"budget={ENTRY_ADVISORY_BUDGET:.2f} "
              f"actual={_TIMING_SECONDS_FMT.format(span)}")
    print(verdict)
    seconds = _TIMING_SECONDS_FMT.format(span)
    print(f"TIME {name} {seconds}")
    if record_file:
        print(f"MEASURE {name} {seconds} form=e2e-serial",
              file=record_file)


def main():
    global ENTRY_ADVISORY_BUDGET
    args, timeout = _split_timeout_arg(sys.argv[1:])
    args, record_out = _split_record_out_arg(args)
    args, config, table_arg = _split_gate_flag_arg(args)
    timeout_fallback = (timeout if timeout is not None
                        else DEFAULT_TIMEOUT_FALLBACK_S)
    ncc = args[0] if len(args) > 0 else DEFAULT_NCC
    nvm = args[1] if len(args) > 1 else DEFAULT_NVM
    #Exactly three positional tools, no more: a silent extra argument
    #would quietly do nothing -- the cuda double-run leg passed its
    #entry list positionally for a whole merge cycle while the runner
    #read manifests only from NLANG_E2E_MANIFEST, and ran the full
    #tour believing it had filtered (fail loud, not silent).
    if len(args) > 3:
        print("ERROR: unexpected extra positional arguments after the "
              "three tool paths: %r (to run a subset manifest, export "
              "NLANG_E2E_MANIFEST=<path>)" % args[3:])
        sys.exit(2)
    #dbg_ tests run under the debugger. An explicit third argument is
    #the ndb to use — gates pass it so an ASan/Debug sweep runs dbg_
    #tests under the same config's debugger (the derived Release path
    #would silently swap in a non-instrumented binary). Default: each
    #tool builds into its own build/src/tools/<tool>/Release directory,
    #so ndb sits two levels over from the nvm binary (build-tree
    #layout), not next to it.
    tools_dir = os.path.dirname(os.path.dirname(os.path.dirname(nvm)))
    ndb = args[2] if len(args) > 2 else os.path.join(
        tools_dir, 'ndb', 'Release', 'ndb.exe')
    _fail_on_stale_tools(ncc, nvm, ndb)
    #Gate-time budget pricing: the judging config comes from --config
    #(the legs derive it from the reference key they sweep) or, missing
    #that, from the two-way tool-path guess the retired config branch
    #used. The table loads once here — it is the repository's and the
    #legs', not hand-typed — so a malformed row or an unknown --config
    #stops the run visibly before any entry work instead of silently
    #running the historical flat cap.
    try:
        config_key, guessed = _resolve_judging_config(config, ncc)
    except ValueError as e:
        print(f"ERROR: {e}")
        sys.exit(2)
    table_path = table_arg or os.path.join(REPO_ROOT, 'tests',
                                           'gate_time_reference.txt')
    try:
        gate_table = _load_gate_time_table()
        gate_rows = gate_table.effective_rows(table_path)
    except (OSError, ValueError) as e:
        print(f"ERROR: gate-time table {table_path}: {e}")
        sys.exit(2)
    scale_env = os.environ.get('NLANG_GATE_MACHINE_SCALE')
    try:
        #Budget-only scaling (D-4/D-8): the scalar is this machine's
        #calibration, the work factor the soak knob; a missing scalar
        #prices at SCALE_BAND_MAX (the widest safe band) and says so.
        scalar = (float(scale_env) if scale_env
                  else float(gate_table.SCALE_BAND_MAX))
        work_factor = float(os.environ.get('NLANG_TEST_WORK_FACTOR')
                            or gate_table.DEFAULT_WORK_FACTOR)
    except ValueError as e:
        print(f"ERROR: {e} (NLANG_GATE_MACHINE_SCALE / "
              f"NLANG_TEST_WORK_FACTOR)")
        sys.exit(2)
    if guessed:
        print(f"TIMEOUT judging config={config_key} "
              f"(构型按路径猜为{guessed}，--config=Debug|ASan|Release可显式指定)")
    else:
        print(f"TIMEOUT judging config={config_key}")
    if not scale_env:
        band = (scalar * max(work_factor, 1.0)
                * gate_table.SAFETY_MULTIPLIER)
        #The old flat cap priced back through this band is the reference
        #the startup line quotes (11.25 s at the default band: 90 / (4*2);
        #a work factor above 1 widens the band and shrinks it further)
        #— derived, so a custom work factor keeps the arithmetic honest.
        short_ref = DEFAULT_TIMEOUT_FALLBACK_S / band
        print(f"TIMEOUT budget: 未做本机校准（缺标量NLANG_GATE_MACHINE_SCALE，"
              f"按SCALE_BAND_MAX计价）: 预算=参考秒数×{band:g}，"
              f"参考秒数{short_ref:g}对应旧Debug平帽90s；处置是经门禁跑完成校准，"
              f"或导出NLANG_GATE_MACHINE_SCALE")

    if not os.path.isfile(MANIFEST):
        print(f"ERROR: manifest.txt not found at {MANIFEST}")
        sys.exit(1)

    env_gates = {}
    try:
        env_gates = parse_env_gates(ENV_GATES_FILE)
    except ValueError as e:
        print(f"ERROR: {e}")
        sys.exit(2)
    keys_needed = set()
    for keys in env_gates.values():
        keys_needed |= keys
    env_state = probe_env(keys_needed)

    passed = 0
    failed = 0
    skipped = 0
    warnings = []
    errors = []

    #Sidecar coverage: every <name>.run.stderr must key a manifest
    #entry, else its diagnostic assertion is silently skipped. A subset
    #leg (NLANG_E2E_MANIFEST set) sees only its family's entries, so
    #the orphan check would flag every other family's sidecar — suite
    #coverage is a full-tour invariant, skip it here.
    #Override manifests (tier subsets, the cuda double-run list) hold
    #entries of the MAIN tour: their corpora resolve against tests/e2e,
    #so an examples-manifest name would run the wrong corpus file. The
    #override channel therefore validates and prices against the main
    #manifest only — the first read also stopped examples rows from
    #shadowing main-tour rows of the same name (class_basic: expected
    #42 in the tour, 3 in the examples sample). The orphan-sidecar
    #check below still spans both manifests: suite coverage is a
    #full-tour invariant.
    committed_rows = {}
    with open(MANIFEST, encoding='utf-8') as mf:
        for line in mf:
            line = line.strip()
            if line and not line.startswith('#'):
                parts = line.split(None, 2)
                if parts:
                    committed_rows[parts[0]] = (
                        parts[1] if len(parts) > 1 else '')
    known_names = set(committed_rows)
    if os.path.isfile(EXAMPLES_MANIFEST):
        with open(EXAMPLES_MANIFEST, encoding='utf-8') as mf:
            for line in mf:
                line = line.strip()
                if line and not line.startswith('#'):
                    known_names.add(line.split(None, 1)[0])
    #Override-manifest validation: the cuda double-run leg once passed
    #its entry list positionally and the runner silently ignored it; the
    #NLANG_E2E_MANIFEST channel must not repeat that silence. An EMPTY
    #override (nothing runnable) is refused outright — a misspelled row
    #needs no guard, it fails its entry loudly at run time.
    override_path = os.environ.get('NLANG_E2E_MANIFEST')
    if override_path:
        override_names = set()
        with open(override_path, encoding='utf-8') as mf:
            for line in mf:
                line = line.strip()
                if line and not line.startswith('#'):
                    override_names.add(line.split(None, 1)[0])
        if not override_names:
            print(f"ERROR: {override_path}: no runnable rows")
            sys.exit(2)
    if not override_path:
        for orphan in _orphan_run_stderr_sidecars(known_names):
            #Not a per-entry run: no entry work happened, so this FAIL
            #carries no TIME row (per-entry verdicts go via
            #_emit_verdict, this startup check does not).
            print(f"FAIL {orphan} (run.stderr sidecar matches no manifest entry)")
            failed += 1
            errors.append(
                f"  {orphan}: run.stderr sidecar matches no manifest entry")

    #Scratch for this run (per-tag namespace when NLANG_E2E_SCRATCH_TAG is
    #set): created before the first entry, dropped in teardown_scratch.
    prepare_scratch()
    #Raw-measure sink (--record-out): opened after every startup guard
    #so an aborted run leaves no half-written file; one MEASURE row per
    #per-entry verdict, written by _emit_verdict.
    record_file = (open(record_out, 'w', encoding='utf-8')
                   if record_out else None)

    for manifest_path, sources_dir, out_dir in _manifest_configs():
      #The coarse scope that owns this pass's entries: one of the two
      #committed manifests, recognized by file name; a subset or
      #override manifest (NLANG_E2E_MANIFEST) has no scope row of its
      #own, so its entries price through their own row or the
      #`default` row only.
      manifest_scope = os.path.basename(manifest_path)
      if manifest_scope not in gate_table.MANIFEST_SCOPES:
          manifest_scope = None
      with open(manifest_path, 'r', encoding='utf-8') as f:
        #Scratch dir for the examples pass: reset once before its lines
        #(each example writes distinct files, so per-entry reset is not
        #needed); the manifest pass never creates it.
        if out_dir == EXAMPLES_TMP:
            reset_scratch_dir(EXAMPLES_TMP)
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            #Column 1: name; column 2: expected exit code / "compile_error";
            #optional column 3: expected stdout substring (maxsplit=2 keeps
            #spaces inside the substring; absent = no stdout assertion).
            #A single-column row is a bare name (the double-run list's
            #spelling) and prices its verdict against the committed
            #manifest's own expectation — "same exit codes as the
            #pinned-cpu suite" is the list's stated contract. Guessing a
            #0 here once fabricated 103 failures: every throws family
            #(expected 1), compile_error row and the exit-code
            #passthrough corpora (expected 42) read as a mismatch.
            parts = line.split(None, 2)
            if len(parts) < 2:
                parts.append(committed_rows.get(parts[0], '0'))
            name = parts[0]
            #Environment gate: a gated entry whose capability the OS
            #oracle says is absent is skipped before any tooling runs
            #(the reason is authoritative — the machine cannot perform
            #the premise, not a NLang regression).
            gate_reason = env_skip_reason(name, env_gates, env_state)
            if gate_reason:
                print(f"SKIP {name} ({gate_reason})")
                skipped += 1
                continue
            #Per-entry kill point and advisory budget from the table
            #(_price_e2e_entry grades own-row vs coarse-scope vs no
            #row): kill_s drives the four subprocess timeouts below,
            #kill_note labels only a runner kill there, and the coarse
            #advisory rides the ENTRY_ADVISORY_BUDGET global that
            #_emit_verdict reads.
            kill_s, kill_note, advisory = _price_e2e_entry(
                gate_table, gate_rows, config_key, name, manifest_scope,
                work_factor, scalar, not scale_env)
            if kill_s is None:
                kill_s = timeout_fallback
                print(f"TIMEOUT-FALLBACK {name}: no {config_key} row for "
                      f"it in {table_path}, kill = "
                      f"{_TIMING_SECONDS_FMT.format(kill_s)} s")
            ENTRY_ADVISORY_BUDGET = advisory
            #Per-entry timing origin: from here to the entry's verdict
            #line is the entry's own work, which is what the TIME and
            #MEASURE rows report.
            entry_started = time.monotonic()
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
                    _emit_verdict(name, f"FAIL {name} (file missing)",
                                  entry_started, record_file)
                    failed += 1
                    errors.append(f"  {name}: file missing")
                else:
                    print(f"SKIP {name} (file missing)")
                continue

            #Directory-form (multi-module) entries only exist for the
            #tests manifest: they compile .ncu files INTO the source
            #dir, which must never happen under examples/. Multi-module
            #examples are gated by ctest (project_compile/project_run).
            if os.path.isdir(test_dir) and out_dir != SCRIPT_DIR:
                _emit_verdict(
                    name,
                    f"FAIL {name} (directory-form entry not allowed"
                    " in the examples manifest; gate multi-module"
                    " examples via ctest instead)",
                    entry_started, record_file)
                failed += 1
                errors.append(f"  {name}: directory-form entry not "
                              f"allowed in the examples manifest")
                continue

            #Phase 9c cross-module: multi-file tests use a directory layout.
            #Layout: <name>/ contains order.txt (module names in compile
            #order, dependency first, main last) + per-module <module>.n
            #sources. Runner compiles each in order, emits .ncu into the
            #test dir, then runs the last module's .ncu.
            if os.path.isdir(test_dir):
                order_path = os.path.join(test_dir, 'order.txt')
                if not os.path.isfile(order_path):
                    _emit_verdict(
                        name, f"FAIL {name} (missing order.txt in test dir)",
                        entry_started, record_file)
                    failed += 1
                    errors.append(f"  {name}: missing order.txt")
                    continue
                with open(order_path, 'r', encoding='utf-8') as of:
                    modules = [m.strip() for m in of.read().split() if m.strip()]
                if not modules:
                    _emit_verdict(name, f"FAIL {name} (order.txt is empty)",
                                  entry_started, record_file)
                    failed += 1
                    errors.append(f"  {name}: empty order.txt")
                    continue

                #Compile each module in order. Sealed layout (default):
                #each module's source is staged into its own private
                #dir and -I points at a per-test scratch dir holding
                #ONLY the earlier modules' .ncu artifacts — imports
                #resolve against compiled artifacts (the external-stub
                #path), never against the sibling .n sources. Compiling
                #from the test dir itself would let ncc's library
                #source discovery inline the siblings, silently
                #exercising source inlining instead of the package
                #import model the cross-module tests exist to pin.
                #Runtime is unaffected: nvm/ndb default their package
                #search to the entry .ncu's own directory, where the
                #sibling .ncu files still live.
                #
                #Source-inline layout (marker file source_inline.txt):
                #the flow the sealed layout deliberately avoids becomes
                #the point of the test — every module source is staged
                #TOGETHER in one shared dir and each module compiles
                #with -I pointing at it, so import discovery inlines
                #the earlier modules' .n sources as library translation
                #units (the stdlib flow). The .ncu artifacts still land
                #in the shared dir and join the run closure through the
                #entry-dir search default.
                src_inline = os.path.isfile(
                    os.path.join(test_dir, 'source_inline.txt'))
                imports_dir = os.path.join(IMPORTS_TMP, name)
                if os.path.isdir(imports_dir):
                    shutil.rmtree(imports_dir)
                os.makedirs(imports_dir, exist_ok=True)
                compile_ok = True
                reject_stderr = ''  #diagnostics of the failed compile
                for mod_name in modules:
                    if src_inline:
                        src = os.path.join(imports_dir, f"{mod_name}.n")
                        shutil.copyfile(
                            os.path.join(test_dir, f"{mod_name}.n"), src)
                    else:
                        mod_dir = os.path.join(imports_dir, mod_name)
                        os.makedirs(mod_dir, exist_ok=True)
                        src = os.path.join(mod_dir, f"{mod_name}.n")
                        shutil.copyfile(
                            os.path.join(test_dir, f"{mod_name}.n"), src)
                    out = os.path.join(imports_dir, f"{mod_name}.ncu")
                    try:
                        r = subprocess.run(
                            [ncc, 'build', src, '-o', out, '-I', imports_dir],
                            capture_output=True, timeout=kill_s)
                    except Exception as e:
                        _emit_verdict(name,
                                      f"FAIL {name} (compile error: "
                                      f"{e}){_kill_note(e, kill_note)}",
                                      entry_started, record_file,
                                      cap_killed=_is_cap_kill(e))
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
                        #diagnostics. ';' separates expectations; a leading
                        #'!' negates one (must NOT appear).
                        bad = _rejection_violation(expected_stdout,
                                                   reject_stderr)
                        if bad:
                            negate, want = bad
                            _emit_verdict(
                                name, f"FAIL {name} (diagnostic mismatch)",
                                entry_started, record_file)
                            failed += 1
                            errors.append(
                                f"  {name}: expected diagnostic "
                                f"{'NOT containing' if negate else 'containing'} "
                                f"{want!r}; stderr: "
                                f"{reject_stderr[:300]}")
                            continue
                        _emit_verdict(
                            name,
                            f"PASS {name} (compile error as expected)",
                            entry_started, record_file)
                        passed += 1
                        continue
                    _emit_verdict(name, f"FAIL {name} (compilation failed)",
                                  entry_started, record_file)
                    failed += 1
                    errors.append(f"  {name}: compilation failed")
                    continue

                if expected == "compile_error":
                    _emit_verdict(
                        name,
                        f"FAIL {name} (expected compile_error but compiled ok)",
                        entry_started, record_file)
                    failed += 1
                    errors.append(f"  {name}: expected compile_error, compiled")
                    continue

                #Run the last module (the artifacts dir's sibling .ncu
                #files are found via the entry-dir search default)
                main_nmod = os.path.join(imports_dir, f"{modules[-1]}.ncu")
                #dbg_/dbgm_ prefixes: run under ndb instead of nvm,
                #driving it with the <name>.stdin command script (ndb
                #stops at the first statement, so it always needs input).
                stdin_bytes = None
                if _is_debugger_run(name):
                    stdin_path = os.path.join(test_dir, f"{name}.stdin")
                    with open(stdin_path, 'rb') as sf:
                        stdin_bytes = sf.read()
                try:
                    result = subprocess.run(
                        _runner_argv(name, ndb, nvm) + [main_nmod],
                        capture_output=True, timeout=kill_s,
                        input=stdin_bytes,
                        env=_run_env(name, test_dir))
                    actual = result.returncode
                    stderr_text = _matchable_stderr(result.stderr)
                except Exception as e:
                    _emit_verdict(
                        name,
                        f"FAIL {name} (runtime error: "
                        f"{_describe_run_error(e)}){_kill_note(e, kill_note)}",
                        entry_started, record_file,
                        cap_killed=_is_cap_kill(e))
                    failed += 1
                    errors.append(f"  {name}: runtime error: "
                                  f"{_describe_run_error(e)}")
                    continue

                sentinel = _skip_sentinel(_exit_width(actual),
                                          _exit_width(expected), stderr_text)
                if sentinel is not None:
                    if sentinel:
                        print(f"SKIP {name} ({sentinel})")
                        skipped += 1
                    else:
                        _emit_verdict(
                            name,
                            f"FAIL {name} (skip sentinel without a "
                            f"reason)",
                            entry_started, record_file)
                        failed += 1
                        errors.append(f"  {name}: skip sentinel "
                                      f"without a reason")
                    continue

                if _exit_width(actual) == _exit_width(expected):
                    #dbg_/dbgm_ tests: optional stdout-substring
                    #assertion (manifest column 3), mirroring the
                    #single-file path.
                    if expected_stdout:
                        stdout_text = _matchable_stdout(name, result.stdout)
                        if expected_stdout not in stdout_text:
                            _emit_verdict(
                                name,
                                f"FAIL {name} (stdout missing {expected_stdout!r})",
                                entry_started, record_file)
                            failed += 1
                            errors.append(f"  {name}: stdout missing {expected_stdout!r}")
                            continue
                    #v0.8.5: <name>.run.stderr for directory fixtures —
                    #same substring + '!'-negation shape as the
                    #single-file path, reading the fixture dir.
                    run_stderr_path = os.path.join(
                        test_dir, f"{name}.run.stderr")
                    if os.path.isfile(run_stderr_path):
                        with open(run_stderr_path, encoding='utf-8') as sf:
                            want = sf.read().strip()
                        negate = want.startswith('!')
                        want = want.lstrip('!').strip()
                        if (want in stderr_text) == negate:
                            _emit_verdict(
                                name,
                                f"FAIL {name} (run stderr expectation mismatch)",
                                entry_started, record_file)
                            failed += 1
                            errors.append(
                                f"  {name}: expected run stderr "
                                f"{'NOT containing' if negate else 'containing'} "
                                f"{want!r}; stderr: {stderr_text[:300]}")
                            continue
                    _emit_verdict(name, f"PASS {name} (exit={actual})",
                                  entry_started, record_file)
                    passed += 1
                else:
                    _emit_verdict(
                        name,
                        f"FAIL {name} (expected={expected}, actual={actual})",
                        entry_started, record_file)
                    failed += 1
                    errors.append(f"  {name}: expected={expected} actual={actual}")
                    if stderr_text:
                        errors.append(f"    stderr: {stderr_text.splitlines()[0]}")
                continue

            #Manifest pass only: its CWD (tests/e2e/, or the tagged run's
            #private dir) resolves ../temp/fs/... to FS_SCRATCH, so that
            #dir has to exist and be empty when the program starts — one
            #entry must not see another's leftovers. The examples pass
            #owns its own pre-cleaned CWD (see the out_dir handling in
            #main).
            if out_dir == SCRIPT_DIR:
                reset_scratch_dir(FS_SCRATCH)

            # Compile
            #0.7.5: <name>.ncc.flags (if present) supplies extra ncc CLI
            #flags (e.g. --no-warn) — same sibling-file discovery shape
            #as .stdin/.flags.
            ncc_flags_path = os.path.join(sources_dir, f"{name}.ncc.flags")
            ncc_extra = []
            if os.path.isfile(ncc_flags_path):
                with open(ncc_flags_path, encoding='utf-8') as ff:
                    ncc_extra = shlex.split(ff.read().strip(),
                                            comments=True)
            nmod_file = os.path.join(out_dir, f"{name}.ncu")
            #A stale artifact from an earlier run would make a failing
            #compile look successful; the rejection gate owns the slot.
            if expected == "compile_error" and os.path.isfile(nmod_file):
                os.remove(nmod_file)
            #Fixed compile CWD: ncc takes an explicit -o, and a compiler
            #that ignored it would drop the artifact in the CWD — pinned to
            #out_dir so concurrent runs cannot pick each other's up.
            try:
                compile_result = subprocess.run(
                    [ncc, 'build', test_file, '-o', nmod_file] + ncc_extra,
                    capture_output=True, timeout=kill_s, cwd=out_dir)
            except Exception as e:
                _emit_verdict(name,
                              f"FAIL {name} (compile error: "
                              f"{e}){_kill_note(e, kill_note)}",
                              entry_started, record_file,
                              cap_killed=_is_cap_kill(e))
                failed += 1
                errors.append(f"  {name}: compile error: {e}")
                continue

            if not os.path.isfile(nmod_file):
                #No artifact where the pinned compile CWD and -o put it: the
                #compile did not produce one. On a non-zero exit a file that
                #did survive an earlier run (a manual red-light probe, for
                #instance) is stale and must not mask the expected rejection,
                #which is why the slot is deleted before the compile.
                if expected == "compile_error":
                    #For compile_error tests an optional column 3 is the
                    #substring ncc's diagnostics (stderr) must contain —
                    #it pins the rejection reason, not just the failure.
                    #';' separates expectations; a leading '!' negates one
                    #(must NOT appear).
                    if expected_stdout:
                        stderr_text = (compile_result.stderr.decode(
                            'utf-8', errors='replace')
                            if compile_result.stderr else '')
                        bad = _rejection_violation(expected_stdout,
                                                   stderr_text)
                        if bad:
                            negate, want = bad
                            _emit_verdict(
                                name, f"FAIL {name} (diagnostic mismatch)",
                                entry_started, record_file)
                            failed += 1
                            errors.append(
                                f"  {name}: expected diagnostic "
                                f"{'NOT containing' if negate else 'containing'} "
                                f"{want!r}; stderr: "
                                f"{stderr_text[:300]}")
                            continue
                    _emit_verdict(
                        name, f"PASS {name} (compile error as expected)",
                        entry_started, record_file)
                    passed += 1
                    continue
                else:
                    _emit_verdict(
                        name, f"FAIL {name} (compilation failed)",
                        entry_started, record_file)
                    failed += 1
                    errors.append(f"  {name}: compilation failed")
                    continue

            #If we reach here, compilation succeeded. If the manifest said
            #compile_error, that's a FAIL (we expected rejection).
            if expected == "compile_error":
                _emit_verdict(
                    name,
                    f"FAIL {name} (expected compile_error but compiled ok)",
                    entry_started, record_file)
                failed += 1
                errors.append(f"  {name}: expected compile_error, compiled")
                if os.path.isfile(nmod_file):
                    os.remove(nmod_file)
                continue

            #0.7.5: <name>.stderr asserts on the compile-phase stderr of a
            #successfully compiled test (warning-behavior contract — see
            #the module docstring). Applied AFTER the compile_error gates
            #so rejection pinning (manifest column 3) keeps its own path.
            stderr_expect_path = os.path.join(sources_dir, f"{name}.stderr")
            if os.path.isfile(stderr_expect_path):
                with open(stderr_expect_path, encoding='utf-8') as sf:
                    want = sf.read().strip()
                negate = want.startswith('!')
                want = want.lstrip('!').strip()
                got_stderr = (compile_result.stderr.decode(
                    'utf-8', errors='replace')
                    if compile_result.stderr else '')
                if (want in got_stderr) == negate:
                    _emit_verdict(
                        name, f"FAIL {name} (stderr expectation mismatch)",
                        entry_started, record_file)
                    failed += 1
                    errors.append(
                        f"  {name}: expected stderr "
                        f"{'NOT containing' if negate else 'containing'} "
                        f"{want!r}; stderr: {got_stderr[:300]}")
                    if os.path.isfile(nmod_file):
                        os.remove(nmod_file)
                    continue

            # Run
            #One fixed CWD per pass: manifest entries run in tests/e2e/ (or
            #in the scratch tag's own dir, see run_dir_for) so their
            #../temp/fs/... scratch paths resolve, and shipped examples run
            #in their scratch dir because several write files next to their
            #CWD (fs_example/, io_example.txt).
            run_cwd = run_dir_for(out_dir)
            #Phase 11: <name>.stdin (if present) is piped to the program —
            #io.readLine tests drive stdin through it.
            stdin_path = os.path.join(sources_dir, f"{name}.stdin")
            stdin_bytes = None
            if os.path.isfile(stdin_path):
                with open(stdin_path, 'rb') as sf:
                    stdin_bytes = sf.read()
            #String-GC Task 2: <name>.flags (if present) supplies extra nvm
            #CLI flags (e.g. --gc-stress=8) — same discovery shape as .stdin.
            #Appended AFTER the module path (nvm parses --gc-stress from any
            #argument position; appending merely matches the historical form).
            flags_path = os.path.join(sources_dir, f"{name}.flags")
            extra_flags = []
            if os.path.isfile(flags_path):
                with open(flags_path, encoding='utf-8') as ff:
                    #comments=True: '#' starts a comment, so a .flags file
                    #can carry a half-word rationale next to its flags.
                    extra_flags = shlex.split(ff.read().strip(),
                                              comments=True)
            try:
                result = subprocess.run(
                    _runner_argv(name, ndb, nvm) + [nmod_file] + extra_flags,
                    capture_output=True, timeout=kill_s,
                    cwd=run_cwd, input=stdin_bytes,
                    env=_run_env(name, sources_dir))
                actual = result.returncode
                stderr_text = _matchable_stderr(result.stderr)
            except Exception as e:
                _emit_verdict(
                    name, f"FAIL {name} (runtime error: "
                    f"{_describe_run_error(e)}){_kill_note(e, kill_note)}",
                    entry_started, record_file,
                    cap_killed=_is_cap_kill(e))
                failed += 1
                errors.append(f"  {name}: runtime error: "
                              f"{_describe_run_error(e)}")
                if os.path.isfile(nmod_file):
                    os.remove(nmod_file)
                continue

            # Clean up
            if os.path.isfile(nmod_file):
                os.remove(nmod_file)

            sentinel = _skip_sentinel(_exit_width(actual),
                                      _exit_width(expected), stderr_text)
            if sentinel is not None:
                if sentinel:
                    print(f"SKIP {name} ({sentinel})")
                    skipped += 1
                else:
                    _emit_verdict(
                        name, f"FAIL {name} (skip sentinel without a "
                        f"reason)", entry_started, record_file)
                    failed += 1
                    errors.append(f"  {name}: skip sentinel "
                                  f"without a reason")
                continue

            if _exit_width(actual) == _exit_width(expected):
                #Phase 11: optional stdout-substring assertion (manifest
                #column 3). Checked before declaring the pass so a wrong
                #stdout is a FAIL, not a warning.
                if expected_stdout:
                    stdout_text = _matchable_stdout(name, result.stdout)
                    if expected_stdout not in stdout_text:
                        _emit_verdict(
                            name,
                            f"FAIL {name} (stdout missing {expected_stdout!r})",
                            entry_started, record_file)
                        failed += 1
                        errors.append(f"  {name}: stdout missing {expected_stdout!r}")
                        continue
                #<name>.run.stdout asserts on the run-phase stdout with
                #the same '!'-negation as the file below. Negation is
                #the point: a dbgm_ --machine test pins that the
                #--verbose listing stays OFF the protocol channel
                #(stdout), not just that it appears on stderr.
                run_stdout_path = os.path.join(
                    sources_dir, f"{name}.run.stdout")
                if os.path.isfile(run_stdout_path):
                    with open(run_stdout_path, encoding='utf-8') as sf:
                        want = sf.read().strip()
                    negate = want.startswith('!')
                    want = want.lstrip('!').strip()
                    got_stdout = _matchable_stdout(name, result.stdout)
                    if (want in got_stdout) == negate:
                        _emit_verdict(
                            name, f"FAIL {name} (run stdout expectation "
                            f"mismatch)", entry_started, record_file)
                        failed += 1
                        errors.append(
                            f"  {name}: expected run stdout "
                            f"{'NOT containing' if negate else 'containing'} "
                            f"{want!r}")
                        continue
                #0.7.7: <name>.run.stderr asserts on the run-phase stderr
                #(io.eprint's console target — stdout assertions above
                #cannot see it). Same shape and '!'-negation as the
                #compile-phase .stderr file.
                run_stderr_path = os.path.join(
                    sources_dir, f"{name}.run.stderr")
                if os.path.isfile(run_stderr_path):
                    with open(run_stderr_path, encoding='utf-8') as sf:
                        want = sf.read().strip()
                    negate = want.startswith('!')
                    want = want.lstrip('!').strip()
                    if (want in stderr_text) == negate:
                        _emit_verdict(
                            name,
                            f"FAIL {name} (run stderr expectation mismatch)",
                            entry_started, record_file)
                        failed += 1
                        errors.append(
                            f"  {name}: expected run stderr "
                            f"{'NOT containing' if negate else 'containing'} "
                            f"{want!r}; stderr: {stderr_text[:300]}")
                        continue
                _emit_verdict(name, f"PASS {name} (exit={actual})",
                              entry_started, record_file)
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
                _emit_verdict(
                    name,
                    f"FAIL {name} (expected={expected}, actual={actual})",
                    entry_started, record_file)
                failed += 1
                errors.append(f"  {name}: expected={expected} actual={actual}")
                #Same shape as the directory-fixture path above: a failing
                #run's stderr is the only record of *why* — the test's own
                #io.eprint diagnosis, or an uncaught 'Runtime error' whose
                #exit code collides with an explicit return 1. Dropped here,
                #the evidence is gone before anyone reads the log.
                if stderr_text:
                    errors.append(
                        f"    stderr: {stderr_text.splitlines()[0]}")

    #Close the --record-out sink before the final summary: MEASURE rows
    #sit in the file object's block buffer (text mode with a file target
    #is not line-buffered), so this explicit close — not the writes — is
    #what leaves the file holding every row written up to this point no
    #matter how the run ends below.
    if record_file:
        record_file.close()

    #Final cleanup: drop the scratch dirs this run owns.
    teardown_scratch()

    print()
    summary = (f"Results: {passed} passed, {failed} failed, "
               f"{skipped} skipped (env-gated)")
    print(summary)
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
