"""Batch-level freshness preflight for the tiered gates.

A batch consumes pre-built artifacts from several (tree, config)
pairs, and staleness discovered mid-batch voids the whole sweep -- the
session that kept paying for it (a Debug tool predating a regenerated
lexer, a Release toolchain two versions old, native DLLs one ABI
behind their host) is the motivation. This tool runs as the batch's
first leg and refuses to judge anything until every consumed artifact
is newer than the repo's compiled inputs, so a stale tree costs one
rebuild instead of a two-hour re-sweep.

The staleness rule is the e2e runner's guard, split per artifact
class so the over-approximation stays where it was paid for. The
three tools compile the whole product, so their input set is the full
src/ + include/ sweep (the runner's rule verbatim, its lessons
included: a false alarm costs one build, a miss a misdiagnosed gate).
Native DLLs compile their own units and the headers those units
reach through #include lines -- folding the full sweep, or even every
public header, onto them reddens every tree after an edit to a header
no unit includes, and no build refreshes a DLL for a file it does not
depend on (2026-11-24: one compiler header staled seven DLLs in every
tree). Two limits of that rule remain, both older than it: the bound is
one for the whole class although the build relinks per module, so an
edit to one module's own file stales its siblings; and the two inputs
src/native/CMakeLists.txt lists from outside that directory
(StackDump.cpp for nlang_net, the vendored zlib for nlang_gzip) are
judged by no rule. Test executables are presence-only: their
per-target input graph is the build system's, which no mtime sweep
replicates, and an over-approximation there is unactionable -- the
rebuild the finding demands is the one the build system just judged
unnecessary. stdlib/*.n is NOT a tool input (the tools compile it as
data -- mixing it in pressured no-op tool rebuilds and once invited a
timestamp-forging touch); it belongs to the per-tree package-copy
check, and the compiled stdlib packages are judged by the runner's
stale-package rule hoisted here: each at least as new as BOTH the
newest stdlib source and the ncc beside it. The repo's VERSION file
bounds tools and native DLLs alike -- a bump rides the version/ABI they
embed and needs no source edit to be real (a tree not reconfigured since
a bump otherwise greens while shipping the old strings). An empty input
sweep or an empty stdlib refuses to judge at all (nothing could ever
be stale); a file vanishing mid-check is a clean usage error, not a
traceback. The required-tool trio (ncc/nvm/ndb) is judged as a class
per tree config: the docs legs consume only ncc/nvm, but a config
missing ndb is an incomplete build of the toolchain, not a consumable
one.
"""
import argparse
import glob
import os
import re
import sys
from pathlib import Path


class UsageError(Exception):
    """A malformed tree spec on the command line."""


class TreeSpec:
    """One consumed (tree, config) pair; with_tests adds test binaries."""

    def __init__(self, path, config, with_tests):
        self.path = Path(path)
        self.config = config
        self.with_tests = with_tests

    @classmethod
    def parse(cls, word, with_tests=False):
        #Config joins the path with a colon -- the only separator that
        #cannot appear inside either half on this project's hosts. The
        #split is on the LAST colon, and the config half must be free
        #of separators, or a Windows drive colon would swallow the
        #whole path as a "config".
        path, sep, config = word.rpartition(":")
        if not (sep and path and config) or set(config) & set("/:\\"):
            raise UsageError(
                "tree spec must be <path>:<config>, got %r" % word)
        return cls(path, config, with_tests)


def _newest(paths):
    """(mtime, relpath) of the newest existing path, (0.0, '') if none."""
    best_mtime, best_name = 0.0, ""
    for path in paths:
        mtime = os.path.getmtime(path)
        if mtime > best_mtime:
            best_mtime, best_name = mtime, path
    return best_mtime, best_name


def repo_inputs(repo):
    """The compiled-input sweep: src/ and include/ sources only."""
    return [path
            for pattern in ("src/**/*.cpp", "src/**/*.hpp", "src/**/*.h",
                            "include/**/*.h", "include/**/*.hpp")
            for path in glob.glob(str(repo / pattern), recursive=True)]


#The units the native DLLs compile. Two inputs src/native/CMakeLists.txt
#lists from outside that directory are not among them and are judged by
#no rule: StackDump.cpp in nlang_net's target_sources and the vendored
#zlib in nlang_gzip's.
NATIVE_SOURCE_PATTERNS = ("src/native/**/*.cpp", "src/native/**/*.h",
                          "src/native/**/*.hpp")
INCLUDE_LINE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)


def native_sources(repo):
    """The units the native DLLs compile."""
    return [path
            for pattern in NATIVE_SOURCE_PATTERNS
            for path in glob.glob(str(repo / pattern), recursive=True)]


def include_closure(repo, roots):
    """(files, unresolved): the roots and every file they reach through
    #include lines; unresolved names the includes that resolve to
    neither include/ nor the including file's directory -- system
    headers, and for a project header reached through some other
    include directory a hole the self-test guards. A quoted name is
    searched beside its includer first, an angled one under include/
    only -- the compiler's search for this tree's conventions; the
    self-test trips on anything the two rules cannot resolve."""
    repo = Path(repo)
    files, unresolved, pending, seen = [], [], list(roots), set()
    while pending:
        path = Path(pending.pop())
        if path in seen:
            continue
        seen.add(path)
        files.append(str(path))
        text = path.read_text(encoding="latin-1")
        for quote, name in INCLUDE_LINE.findall(text):
            public = repo / "include" / name
            candidates = ([path.parent / name, public]
                          if quote == '"' else [public])
            target = next((one for one in candidates if one.is_file()),
                          None)
            if target is not None:
                pending.append(target)
            elif quote == '"' or name.startswith("nlang/"):
                unresolved.append(name)
    return files, sorted(set(unresolved))


def native_inputs(repo):
    """The native DLLs' own units plus the headers those units reach."""
    return include_closure(repo, native_sources(repo))[0]


#The sources the stdlib packages compile: the packing target's own globs
#(CMakeLists.txt, nlang_stdlib_package), mirrored into the tree at the
#same relative path -- the top-level units and the web package's dotted
#sub-module arm.
STDLIB_SOURCE_PATTERNS = ("stdlib/*.n", "stdlib/web/*.n")


def stdlib_sources(repo):
    """Every stdlib source the tree's packages compile."""
    return [path
            for pattern in STDLIB_SOURCE_PATTERNS
            for path in glob.glob(str(repo / pattern))]


def _stale(findings, newest, artifacts):
    for artifact in artifacts:
        if os.path.getmtime(artifact) < newest[0]:
            findings.append(
                "STALE %s is older than %s" % (artifact, newest[1]))


def _spec_artifacts(spec):
    """(tools, dlls, test exes) globbed for one tree config."""
    tools = sorted(glob.glob(
        str(spec.path / "src" / "tools" / "*" / spec.config / "*.exe")))
    dlls = sorted(glob.glob(
        str(spec.path / "native" / spec.config / "*.dll")))
    tests = (sorted(glob.glob(
        str(spec.path / "tests" / spec.config / "*.exe")))
        if spec.with_tests else [])
    return tools, dlls, tests


def check(repo, specs):
    """Findings (empty = fresh) for every spec's consumed artifacts."""
    repo = Path(repo)
    input_files = repo_inputs(repo)
    if not input_files:
        raise UsageError(
            "no compiled inputs under %s -- the src/ + include/ sweep "
            "is empty, so nothing could ever be judged stale" % repo)
    stdlib_modules = sorted(stdlib_sources(repo))
    if not stdlib_modules:
        raise UsageError(
            "no stdlib modules under %s -- the package rule has "
            "nothing to judge" % (repo / "stdlib"))
    native_files = native_inputs(repo)
    if not native_files:
        raise UsageError(
            "no native units under %s -- the native DLL rule has "
            "nothing to judge" % (repo / "src" / "native"))
    #A VERSION bump rides the version/ABI every tool and native DLL
    #embeds, with or without a source edit accompanying it: the file
    #bounds both classes, closing the bump-without-reconfigure channel
    #(CMAKE_CONFIGURE_DEPENDS only re-runs at the next build, and the
    #T0/T1 batches never build).
    version_file = repo / "VERSION"
    version_paths = [version_file] if version_file.is_file() else []
    tool_newest = _newest(input_files + version_paths)
    native_newest = _newest(native_files + version_paths)
    stdlib_newest = _newest(stdlib_modules)
    findings = []
    for spec in specs:
        tools, dlls, tests = _spec_artifacts(spec)
        _stale(findings, tool_newest, tools)
        #The exact filename decides: a decoy beside the tool (ncc-old)
        #or an unrelated path segment that merely spells the tool's
        #name must not satisfy the requirement.
        ncc_exe = next((tool for tool in tools
                        if os.path.basename(tool) == "ncc.exe"), None)
        for required in ("ncc", "nvm", "ndb"):
            if not any(os.path.basename(tool) == required + ".exe"
                       for tool in tools):
                findings.append(
                    "MISSING %s binary under %s (%s)"
                    % (required, spec.path / "src" / "tools" / "*"
                       / spec.config,
                       "build the tree before gating"))
        _stale(findings, native_newest, dlls)
        #The package rule: each repo stdlib module must exist as a
        #same-or-newer copy in the tree (data, not compiled input).
        for module in stdlib_modules:
            copy = spec.path / Path(module).relative_to(repo)
            if not copy.is_file():
                findings.append(
                    "MISSING %s (the tree's stdlib package predates it)"
                    % copy)
            elif os.path.getmtime(copy) < os.path.getmtime(module):
                findings.append(
                    "STALE %s is older than %s" % (copy, module))
        #The compiled packages: the runner's stale-package rule hoisted
        #to leg one -- each at least as new as BOTH the newest stdlib
        #source and the ncc that packs it (older than ncc = packed by a
        #superseded compiler). The tree's generated projects name the
        #package set (the configure writes one <domain>.nproj per domain
        #from the same table as the packing commands), so the set is read
        #rather than duplicated here; a tree without projects (an
        #installed tool tree ships the packages only) is not judged.
        for project in sorted(glob.glob(
                str(spec.path / "stdlib" / "*.nproj"))):
            npkg = os.path.splitext(project)[0] + ".npkg"
            if not os.path.isfile(npkg):
                findings.append(
                    "MISSING %s (the tree's configure expects it; build "
                    "the nlang_stdlib_package target)" % npkg)
                continue
            npkg_mtime = os.path.getmtime(npkg)
            bounds = [(stdlib_newest[0], stdlib_newest[1])]
            if ncc_exe is not None:
                bounds.append((os.path.getmtime(ncc_exe), ncc_exe))
            for bound, bound_name in bounds:
                if npkg_mtime < bound:
                    findings.append(
                        "STALE %s is older than %s (rebuild the "
                        "nlang_stdlib_package target)" % (npkg, bound_name))
        candidates = []
        if spec.with_tests:
            candidates = tests
            if not candidates:
                findings.append(
                    "MISSING test binaries under %s (a ctest tree must "
                    "be built before gating)"
                    % (spec.path / "tests" / spec.config))
        #A version bump regenerates the tree's header at configure; a
        #tool or native DLL older than it predates the bump's build
        #(they embed the version/ABI). Test binaries are presence-only:
        #their per-target input graph is the build system's, which no
        #mtime sweep replicates, and a header/src over-approximation
        #here is unactionable -- the rebuild the finding demands is the
        #same one the build system just judged unnecessary. Gross
        #version drift still reaches them: a bump rides source changes
        #that stale the tools, and the rebuild those findings demand
        #relinks every affected test target.
        version_header = spec.path / "generated" / "nlang_version.h"
        if version_header.is_file():
            header_mtime = os.path.getmtime(version_header)
            for artifact in tools + dlls:
                if os.path.getmtime(artifact) < header_mtime:
                    findings.append(
                        "STALE %s is older than %s"
                        % (artifact, version_header))
    return findings


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="gate_freshness",
        description="Refuse to judge a gate batch whose consumed "
                    "(tree, config) artifacts predate the repo's "
                    "compiled inputs.")
    sub = parser.add_subparsers(dest="command", required=True)
    p_check = sub.add_parser(
        "check", help="judge every named (tree, config) pair")
    p_check.add_argument("--repo", required=True,
                         help="repository root (source of the input sweep)")
    p_check.add_argument("--tree", action="append", default=[],
                         metavar="PATH:CONFIG",
                         help="a consumed tree config: tools + native DLLs "
                              "+ stdlib packages are judged")
    p_check.add_argument("--ctest-tree", action="append", default=[],
                         metavar="PATH:CONFIG",
                         help="like --tree, and the test executables the "
                             "ctest legs run are judged too")
    opts = parser.parse_args(argv)
    try:
        specs = ([TreeSpec.parse(w, False) for w in opts.tree]
                 + [TreeSpec.parse(w, True) for w in opts.ctest_tree])
        if not specs:
            raise UsageError("name at least one --tree/--ctest-tree")
        findings = check(opts.repo, specs)
    except UsageError as err:
        print("gate_freshness: %s" % err, file=sys.stderr)
        return 2
    except FileNotFoundError as err:
        print("gate_freshness: %s disappeared while checking"
              % err, file=sys.stderr)
        return 2
    for finding in findings:
        print("gate_freshness: %s" % finding, file=sys.stderr)
    if findings:
        print("gate_freshness: %d finding(s): rebuild before gating"
              % len(findings), file=sys.stderr)
        return 1
    artifacts = sum(sum(len(group) for group in _spec_artifacts(spec))
                    for spec in specs)
    print("gate_freshness: %d artifacts fresh across %d tree-config(s)"
          % (artifacts, len(specs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
