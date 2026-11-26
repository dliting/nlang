#!/usr/bin/env python3
"""NLang Phase 2 end-to-end test runner.

Usage:
    conda run -n py313 python run_e2e_tests.py [ncc_path] [nvm_path]
        [ndb_path] [filter_file] [--timeout=SEC]

Every run pins NLANG_TENSOR_BACKEND (default `cpu`) so a machine with a
GPU cannot silently move the suite onto the device; set
NLANG_E2E_TENSOR_BACKEND=mirror|cuda for the device double-runs.

Reads manifest.txt (name + expected_exit_code [+ optional expected stdout
substring]), compiles each <name>.n, runs the .ncu, and reports results.
--timeout=SEC raises the flat 30 s per-test subprocess cap for this
invocation only: the ASan-instrumented tree runs ~7x slower than Release,
and its gate run needs more headroom for the pattern-budget case
(stdlib_regex_size crosses the size limit with 25000 literal atoms).
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
_examples_tmp/ (their run CWD), removed at end of run.
Reads env_gates.txt (when present): `name <capability...>` rows gate an
entry behind a machine capability (icmp: OS loopback echo answers; dns:
reserved-TLD NXDOMAIN honesty). When the out-of-band OS oracle says the
capability is absent (VPN filter drivers, DNS hijacks), the entry is
skipped with a printed SKIP line — counted, never silently green.
Skip sentinel: a fixture whose case the machine cannot serve (e.g. a
two-device test on a single-GPU box) exits E2E_SKIP_EXIT naming the
reason on stderr; the runner reports SKIP with that reason (a sentinel
without a reason is a FAIL — see E2E_SKIP_EXIT).
"""

import os
import sys
import glob
import socket
import subprocess
import shutil
import shlex

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, '..', '..'))
DEFAULT_NCC = os.path.join(
    SCRIPT_DIR, '..', '..', 'build', 'src', 'tools', 'ncc', 'Release', 'ncc.exe')
DEFAULT_NVM = os.path.join(
    SCRIPT_DIR, '..', '..', 'build', 'src', 'tools', 'nvm', 'Release', 'nvm.exe')
MANIFEST = os.path.join(SCRIPT_DIR, 'manifest.txt')
#Flat per-test subprocess cap (compile and run), overridable per invocation
#with --timeout=SEC (see the module docstring).
TIMEOUT_SEC = 30
#Skip sentinel (multi-GPU spec's cross-platform ruling): a fixture that
#cannot run its case on this machine — a device census too small, a
#capability absent — prints the reason to stderr and exits this code;
#the manifest still expects the ordinary success (0). The runner turns
#it into a visible SKIP with the reason (the case ran nothing, but the
#compile ran and the contract held); a sentinel without a reason is a
#FAIL — a silent skip could mask a real breakage.
E2E_SKIP_EXIT = 90


def _skip_sentinel(actual, expected, stderr_text):
    """Skip-sentinel verdict for one finished run: None when the entry
    is not on the sentinel path, else the reason string ('' when the
    sentinel arrived without naming one)."""
    if actual != E2E_SKIP_EXIT or expected == E2E_SKIP_EXIT:
        return None
    return next((ln.strip() for ln in stderr_text.splitlines()
                 if ln.strip()), '')
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
PHASE8_TMP = os.path.join(SCRIPT_DIR, '_phase8_tmp')
PHASE11_TMP = os.path.join(SCRIPT_DIR, '_p11_tmp')
#Per-test artifact dir for multi-module imports (see the directory-form
#compile block): holds ONLY the earlier modules' .ncu files.
IMPORTS_TMP = os.path.join(SCRIPT_DIR, '_imports_tmp')

#Shipped examples (examples/) run through the same compile+run gate.
#Entries resolve against EXAMPLES_DIR; their .ncu/stdin/scratch all
#live under EXAMPLES_TMP so the source examples/ never gains build
#artifacts (fs-writing examples get an isolated, pre-cleaned CWD).
EXAMPLES_MANIFEST = os.path.join(SCRIPT_DIR, 'examples_manifest.txt')
EXAMPLES_DIR = os.path.normpath(
    os.path.join(SCRIPT_DIR, '..', '..', 'examples'))
EXAMPLES_TMP = os.path.join(SCRIPT_DIR, '_examples_tmp')


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

    The native modules (src/native) are dlopen'd shared libraries the
    tools load at runtime, not statically linked into them, so a native
    source change rebuilds only that module's .so, not the tools. The
    tool check below therefore excludes src/native — including it would
    alarm on a native edit, and the no-op tool rebuild the alarm demands
    never clears it because the tools' mtime does not advance. A stale
    .so is caught by its own check below.
    """
    def _is_native_module_source(path):
        # True when path lies under <repo>/src/native (the dlopen'd
        # modules). The glob below is anchored at REPO_ROOT, so the
        # repo-relative components are the tool's own inputs (src/*)
        # versus the dlopen'd modules (src/native/*).
        parts = os.path.relpath(path, REPO_ROOT).split(os.sep)
        return (len(parts) >= 2 and parts[0] == 'src'
                and parts[1] == 'native')

    newest_src, newest_name = _newest_file(
        path
        for pattern in ('src/**/*.cpp', 'src/**/*.hpp', 'src/**/*.h',
                        'include/**/*.h', 'include/**/*.hpp')
        for path in glob.glob(os.path.join(REPO_ROOT, pattern),
                              recursive=True)
        if not _is_native_module_source(path))
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
    #Native-module staleness: each module is a dlopen'd shared library
    #the tools load at runtime, so it must be at least as new as its own
    #sources — a stale .so would be loaded by a fresh tool and misreport
    #module behavior as a test bug. The newest .so is bound against the
    #newest native source (over-approximation, as above: a false alarm
    #costs one native-module rebuild, which clears it). The .so live in
    #the build's native/ directory, three levels up from ncc's own dir.
    native_dir = os.path.normpath(os.path.join(
        os.path.dirname(ncc), '..', '..', '..', 'native'))
    newest_so, newest_so_name = _newest_file(
        glob.glob(os.path.join(native_dir, 'libnlang_*.so')) +
        glob.glob(os.path.join(native_dir, 'nlang_*.dll')))
    newest_native_src, newest_native_name = _newest_file(
        glob.glob(os.path.join(REPO_ROOT, 'src', 'native', '**', '*.cpp'),
                  recursive=True))
    if newest_so_name and newest_so < newest_native_src:
        print(f"ERROR: {_display_path(newest_so_name)} is older "
              f"than {newest_native_name} - rebuild the native module "
              f"before running the suite (a stale .so is loaded by a "
              f"fresh tool)")
        sys.exit(2)
    #Load-time closure hazard: any .npkg in the tests directory is on
    #the module-local search path and shadows the system stdlib package
    #for every module run from here (an experiment leftover here once
    #made manual runs pass while the harness failed).
    for pkg in glob.glob(os.path.join(SCRIPT_DIR, '*.npkg')):
        print(f"ERROR: {pkg} shadows the system stdlib for modules run "
              f"from tests/e2e; remove it")
        sys.exit(2)
    #Stale-package hazard: stdlib.npkg is compiled by ncc from the
    #stdlib/*.n sources (the nlang_stdlib_package step), so the package
    #must be at least as new as BOTH of its inputs — a package older
    #than the newest source misses the edit (the 2026-10-31 redesign ran
    #green against a pre-flip npkg for a full task), and a package older
    #than ncc was built by a superseded compiler. Derived from the ncc
    #path (<build>/src/tools/ncc/<cfg>/ncc.exe); skipped when no npkg
    #sits at that layout (an installed tool tree ships its package
    #beside the binaries, not under a build root).
    build_root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.dirname(ncc)))))
    npkg = os.path.join(build_root, 'stdlib', 'stdlib.npkg')
    if os.path.isfile(npkg):
        newest_n, newest_n_name = _newest_file(
            glob.glob(os.path.join(REPO_ROOT, 'stdlib', '*.n')))
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

#Tests that need CWD=SCRIPT_DIR for their relative file paths, plus a
#scratch dir pre-cleaned before and removed after the run.
def _needs_script_cwd(name):
    return (name.startswith('file_stream_') or name.startswith('fs_struct_')
            or name.startswith('fs_object_') or name.startswith('stdlib_io_')
            or name.startswith('stdlib_fs_')
            or name.startswith('stream_long_'))

#Test name suffixes that mark intentional throw-tests. Tests ending in
#these suffixes are excluded from the P3.7 hidden-throw detector: their
#exit=1 is by design (runtime throws → exit 1 = expected 1).
INTENTIONAL_THROW_SUFFIXES = (
    '_throws', '_throw', '_null', '_oob', '_bounds', '_eos',
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


def _is_debug_config(ncc):
    """True when the tools were built in the Debug config.

    Windows multi-config generators put the config in the tool path
    (build/src/tools/ncc/Release/ncc.exe). Single-config generators
    (Linux Makefiles/Ninja) record it in CMakeCache.txt instead — the
    tool path there carries no config component, so a path-shape check
    alone would silently never fire the Debug scaling on them.
    """
    if '/Debug/' in ncc.replace(os.sep, '/'):
        return True
    cache_path = os.path.normpath(os.path.join(
        os.path.dirname(ncc), '..', '..', '..', 'CMakeCache.txt'))
    try:
        with open(cache_path, encoding='utf-8') as cache:
            for line in cache:
                if line.startswith('CMAKE_BUILD_TYPE'):
                    return 'Debug' in line.split('=', 1)[1]
    except OSError:
        pass    #no cache readable: assume the path shape said enough
    return False


def _test_runtime_cap(name, sources_dir, effective_timeout):
    """Runtime subprocess cap for one test: <name>.timeout raises it.

    Same sibling-file discovery shape as .stdin/.flags. The file holds
    the test's needed budget in seconds and may only RAISE the cap —
    the effective value (the 30s default, the invocation's
    --timeout=SEC, scaled x3 under Debug) is the floor, so an
    invocation-explicit calibration (the gate's --timeout=120) is
    never silently reduced by a test-side file.
    """
    timeout_path = os.path.join(sources_dir, f"{name}.timeout")
    if not os.path.isfile(timeout_path):
        return effective_timeout
    try:
        with open(timeout_path, encoding='utf-8') as tf:
            requested = int(tf.read().strip())
    except (OSError, ValueError):
        return effective_timeout
    return max(requested, effective_timeout)


def main():
    global TIMEOUT_SEC
    # POSIX: raise the per-process file-descriptor limit before the suite.
    # A test that opens many concurrent connections in one process
    # (net_conc_bench: 1000 clients + 1000 server accepts) needs more than
    # the default 1024. The limit is inherited by every child the harness
    # spawns, so raising it once here covers the whole run. Best-effort:
    # a container may cap the hard limit below the target, in which case
    # the raise is refused and the default limit still applies.
    if os.name == 'posix':
        import resource
        try:
            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            resource.setrlimit(
                resource.RLIMIT_NOFILE,
                (min(max(soft, 4096), hard), hard))
        except (ValueError, OSError):
            pass
    args, timeout = _split_timeout_arg(sys.argv[1:])
    if timeout is not None:
        TIMEOUT_SEC = timeout
    ncc = args[0] if len(args) > 0 else DEFAULT_NCC
    nvm = args[1] if len(args) > 1 else DEFAULT_NVM
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
    #Optional fourth argument: a filter file restricting the run to the
    #named manifest entries (one name per line, '#' comments). The
    #device double-run legs (tools/e2e_cuda_double_run.txt, spec D7)
    #pin an explicit subset so a device run stays inside the 3-minute
    #per-test cap; the full-suite regression net stays with the cpu
    #mode. A name in the file that keys no manifest entry fails the
    #run — a stale list would otherwise shrink the net silently.
    #Read from the filtered `args`, not the raw argv: a '--timeout=SEC'
    #flag after the three tool paths would otherwise occupy the filter
    #slot (sys.argv[4]) and be mistaken for a filter file.
    filter_path = args[3] if len(args) > 3 else None
    _fail_on_stale_tools(ncc, nvm, ndb)
    #Debug (and ASan, which builds the Debug config) runs several times
    #slower than Release: asserts on, no optimization, gc-stress
    #amplified. A flat 30s cap that is generous in Release pushes the
    #slowest stress tests against their ceiling under Debug, turning
    #any concurrent machine load into timeout flakes. Scale the
    #per-test cap by config, read off the tool path; x3 keeps every
    #test far from its limit and stays under the 3-minute project cap.
    if _is_debug_config(ncc):
        TIMEOUT_SEC *= 3

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
    known_names = set()
    for manifest_path, _, _ in _manifest_configs():
        with open(manifest_path, encoding='utf-8') as mf:
            for line in mf:
                line = line.strip()
                if line and not line.startswith('#'):
                    known_names.add(line.split(None, 1)[0])
    if not os.environ.get('NLANG_E2E_MANIFEST'):
        for orphan in _orphan_run_stderr_sidecars(known_names):
            print(f"FAIL {orphan} (run.stderr sidecar matches no manifest entry)")
            failed += 1
            errors.append(
                f"  {orphan}: run.stderr sidecar matches no manifest entry")

    filter_names = None
    if filter_path is not None:
        with open(filter_path, encoding='utf-8') as ff:
            filter_names = set()
            for line in ff:
                line = line.strip()
                if line and not line.startswith('#'):
                    filter_names.add(line)
        for stale in sorted(filter_names - known_names):
            print(f"FAIL {stale} (filter names no manifest entry)")
            failed += 1
            errors.append(
                f"  {stale}: filter names no manifest entry")
        if failed:
            sys.exit(1)

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
            if filter_names is not None and name not in filter_names:
                continue
            #Environment gate: a gated entry whose capability the OS
            #oracle says is absent is skipped before any tooling runs
            #(the reason is authoritative — the machine cannot perform
            #the premise, not a NLang regression).
            gate_reason = env_skip_reason(name, env_gates, env_state)
            if gate_reason:
                print(f"SKIP {name} ({gate_reason})")
                skipped += 1
                continue
            #Expected may be an int (runtime exit code) or the literal
            #"compile_error" to assert that ncc rejects the source.
            if parts[1] == "compile_error":
                expected = "compile_error"
            else:
                expected = int(parts[1])
                #A process exit status is only 8 bits on POSIX (the kernel
                #reports result & 0xFF to the parent), so a full-value
                #expectation (exit_code_full_passthrough's 300) is reachable
                #on Windows but not on Linux. Mask to the platform's visible
                #range so the assertion stays meaningful; the low byte still
                #discriminates correct logic from a wrong value.
                if os.name != 'nt':
                    expected &= 0xFF
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
            #tests manifest: they compile .ncu files INTO the source
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
            #sources. Runner compiles each in order, emits .ncu into the
            #test dir, then runs the last module's .ncu.
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
                        #diagnostics. ';' separates expectations; a leading
                        #'!' negates one (must NOT appear).
                        bad = _rejection_violation(expected_stdout,
                                                   reject_stderr)
                        if bad:
                            negate, want = bad
                            print(f"FAIL {name} (diagnostic mismatch)")
                            failed += 1
                            errors.append(
                                f"  {name}: expected diagnostic "
                                f"{'NOT containing' if negate else 'containing'} "
                                f"{want!r}; stderr: "
                                f"{reject_stderr[:300]}")
                            continue
                        print(f"PASS {name} (compile error as expected)")
                        passed += 1
                        continue
                    print(f"FAIL {name} (compilation failed)")
                    failed += 1
                    errors.append(f"  {name}: compilation failed")
                    continue

                if expected == "compile_error":
                    print(f"FAIL {name} (expected compile_error but compiled ok)")
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
                        capture_output=True,
                        timeout=_test_runtime_cap(name, test_dir,
                                                  TIMEOUT_SEC),
                        input=stdin_bytes, env=_run_env(name, test_dir))
                    actual = result.returncode
                    stderr_text = _matchable_stderr(result.stderr)
                except Exception as e:
                    print(f"FAIL {name} (runtime error: "
                          f"{_describe_run_error(e)})")
                    failed += 1
                    errors.append(f"  {name}: runtime error: "
                                  f"{_describe_run_error(e)}")
                    continue

                sentinel = _skip_sentinel(actual, expected, stderr_text)
                if sentinel is not None:
                    if sentinel:
                        print(f"SKIP {name} ({sentinel})")
                        skipped += 1
                    else:
                        print(f"FAIL {name} (skip sentinel without a "
                              f"reason)")
                        failed += 1
                        errors.append(f"  {name}: skip sentinel "
                                      f"without a reason")
                    continue

                if actual == expected:
                    #dbg_/dbgm_ tests: optional stdout-substring
                    #assertion (manifest column 3), mirroring the
                    #single-file path.
                    if expected_stdout:
                        stdout_text = _matchable_stdout(name, result.stdout)
                        if expected_stdout not in stdout_text:
                            print(f"FAIL {name} (stdout missing {expected_stdout!r})")
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
                            print(f"FAIL {name} (run stderr expectation mismatch)")
                            failed += 1
                            errors.append(
                                f"  {name}: expected run stderr "
                                f"{'NOT containing' if negate else 'containing'} "
                                f"{want!r}; stderr: {stderr_text[:300]}")
                            continue
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
            try:
                compile_result = subprocess.run(
                    [ncc, 'build', test_file, '-o', nmod_file] + ncc_extra,
                    capture_output=True, timeout=TIMEOUT_SEC)
            except Exception as e:
                print(f"FAIL {name} (compile error: {e})")
                failed += 1
                errors.append(f"  {name}: compile error: {e}")
                continue

            if not os.path.isfile(nmod_file):
                # ncc may output .ncu in CWD; try looking there. Only a
                # successful compile can have produced it — on a non-zero
                # exit the file is a stale artifact from an earlier run
                # (a manual red-light probe, for instance) and must not
                # mask the expected rejection.
                cwd_nmod = os.path.join(os.getcwd(), f"{name}.ncu")
                if compile_result.returncode == 0 and os.path.isfile(cwd_nmod):
                    shutil.move(cwd_nmod, nmod_file)
                elif expected == "compile_error":
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
                            print(f"FAIL {name} (diagnostic mismatch)")
                            failed += 1
                            errors.append(
                                f"  {name}: expected diagnostic "
                                f"{'NOT containing' if negate else 'containing'} "
                                f"{want!r}; stderr: "
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
                    print(f"FAIL {name} (stderr expectation mismatch)")
                    failed += 1
                    errors.append(
                        f"  {name}: expected stderr "
                        f"{'NOT containing' if negate else 'containing'} "
                        f"{want!r}; stderr: {got_stderr[:300]}")
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
                    capture_output=True,
                    timeout=_test_runtime_cap(name, sources_dir,
                                              TIMEOUT_SEC),
                    cwd=run_cwd, input=stdin_bytes,
                    env=_run_env(name, sources_dir))
                actual = result.returncode
                stderr_text = _matchable_stderr(result.stderr)
            except Exception as e:
                print(f"FAIL {name} (runtime error: "
                      f"{_describe_run_error(e)})")
                failed += 1
                errors.append(f"  {name}: runtime error: "
                              f"{_describe_run_error(e)}")
                if os.path.isfile(nmod_file):
                    os.remove(nmod_file)
                continue

            # Clean up
            if os.path.isfile(nmod_file):
                os.remove(nmod_file)

            sentinel = _skip_sentinel(actual, expected, stderr_text)
            if sentinel is not None:
                if sentinel:
                    print(f"SKIP {name} ({sentinel})")
                    skipped += 1
                else:
                    print(f"FAIL {name} (skip sentinel without a reason)")
                    failed += 1
                    errors.append(f"  {name}: skip sentinel "
                                  f"without a reason")
                continue

            if actual == expected:
                #Phase 11: optional stdout-substring assertion (manifest
                #column 3). Checked before declaring the pass so a wrong
                #stdout is a FAIL, not a warning.
                if expected_stdout:
                    stdout_text = _matchable_stdout(name, result.stdout)
                    if expected_stdout not in stdout_text:
                        print(f"FAIL {name} (stdout missing {expected_stdout!r})")
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
                        print(f"FAIL {name} (run stdout expectation "
                              f"mismatch)")
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
                        print(f"FAIL {name} (run stderr expectation mismatch)")
                        failed += 1
                        errors.append(
                            f"  {name}: expected run stderr "
                            f"{'NOT containing' if negate else 'containing'} "
                            f"{want!r}; stderr: {stderr_text[:300]}")
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
    if os.path.isdir(IMPORTS_TMP):
        shutil.rmtree(IMPORTS_TMP)
    if os.path.isdir(EXAMPLES_TMP):
        shutil.rmtree(EXAMPLES_TMP)

    print()
    print(f"Results: {passed} passed, {failed} failed, "
          f"{skipped} skipped (env-gated or skip sentinel)")
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
