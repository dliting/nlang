"""gate_freshness_test.py -- self-test corpus for the batch freshness
preflight (tools/gate_freshness.py): the per-class input rules (tools
carry the full src/include sweep, native DLLs their own units and the
headers those reach through #include lines, test executables
presence-only), the stdlib package-copy rule (dotted sub-modules by
relative path), the per-domain stdlib package rules (a generated
<domain>.nproj must have its package, and the package must be as new as
source and ncc), the VERSION-file bound, the empty-sweep refusals, the
exact-filename required-tool check and the tree-spec parsing.
Stub repos and trees with pinned mtimes keep the corpus hermetic: no
third-party imports, no build, no network. Two tests read the real
tree, to prove the include walker resolves the project's own include
conventions. Run either way:
    python gate_freshness_test.py
    pytest tools/gate_freshness_test.py
Verdict line: "GATE gate_freshness_selftest: PASS" (or FAIL), exit
code follows.
"""
import io
import os
import sys
import tempfile
from contextlib import redirect_stderr
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gate_freshness  # noqa: E402


REPO_INPUTS = ["src/vm/vm_core.cpp", "include/nlang/abi.h", "stdlib/util.n"]

#The native module's unit reaches its headers in the three forms the
#native rule resolves: the public header through the project's
#<nlang/...> path, a same-dir private header through a quoted name, and
#a helper outside src/native through a relative path.
NATIVE_UNIT = (b'#include <nlang/abi.h>\n'
               b'#include "io_slots.h"\n'
               b'#include "../../runtime/NativeTimers.h"\n')
NATIVE_HEADERS = ["src/native/io/io_slots.h", "src/runtime/NativeTimers.h"]
#A public header no native unit includes: the tools compile it, the
#native DLLs do not.
UNREACHED_HEADER = "include/nlang/compiler/ModuleBuilder.h"

REPO = Path(__file__).resolve().parent.parent
#Quoted includes of the real native units that resolve to neither
#include/ nor the unit's own directory: the vendored zlib, reached
#through its own include directory. Anything else here is a project
#header the native rule would silently judge as a system header.
VENDORED_NATIVE_INCLUDES = {"zlib.h"}


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def write(path, content=b"x"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return path


def set_mtime(path, seconds):
    os.utime(str(path), (seconds, seconds))


def make_repo(root):
    for rel in (REPO_INPUTS + NATIVE_HEADERS + [UNREACHED_HEADER]):
        set_mtime(write(root / rel), 100)
    unit = write(root / "src/native/io/io_native.cpp", NATIVE_UNIT)
    set_mtime(unit, 100)
    return root


def make_tree(root, tools=("ncc", "nvm", "ndb"), native=True,
              stdlib=True, tests=False, version=True):
    for tool in tools:
        set_mtime(write(root / "src" / "tools" / tool / "Debug"
                        / ("%s.exe" % tool)), 200)
    if native:
        set_mtime(write(root / "native" / "Debug" / "nlang_io.dll"), 200)
    if stdlib:
        set_mtime(write(root / "stdlib" / "util.n"), 150)
    if version:
        set_mtime(write(root / "generated" / "nlang_version.h"), 150)
    if tests:
        set_mtime(write(root / "tests" / "Debug" / "test_vm.exe"), 200)
    return root


PACKAGE_DOMAIN = "io"


def write_package(tree, domain=PACKAGE_DOMAIN, mtime=250):
    """A generated per-domain project with its compiled package: cmake
    writes the <domain>.nproj at configure time, the build writes the
    package stamped at `mtime` (250 = fresh: newer than the tools at
    200 and the stdlib copy at 150)."""
    set_mtime(write(tree / "stdlib" / ("%s.nproj" % domain)), 150)
    set_mtime(write(tree / "stdlib" / ("%s.npkg" % domain)), mtime)


def write_package_project(tree, domain=PACKAGE_DOMAIN):
    """The generated project alone: a tree configured for the package
    whose packing target has not run since."""
    set_mtime(write(tree / "stdlib" / ("%s.nproj" % domain)), 150)


def run_check(repo, specs):
    return gate_freshness.check(
        repo,
        [gate_freshness.TreeSpec.parse(s) if isinstance(s, str) else s
         for s in specs])


def expect_usage_error(thunk, what):
    try:
        thunk()
    except gate_freshness.UsageError:
        return
    check(False, "expected a UsageError: %s" % what)


def test_fresh_tree_passes():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        check(run_check(repo, ["%s:Debug" % tree]) == [],
              "a fresh tree must produce no findings")


def test_stale_tool_exe_is_flagged_with_the_input_named():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(tree / "src" / "tools" / "ncc" / "Debug" / "ncc.exe", 100)
        set_mtime(repo / "src" / "vm" / "vm_core.cpp", 200)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("ncc.exe" in f and "vm_core.cpp" in f for f in findings),
              "no finding names the tool and its input: %r" % findings)


def test_missing_tool_is_flagged():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo = make_repo(tmp / "repo")
        tree = make_tree(tmp / "tree", tools=("ncc",))
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("nvm" in f for f in findings),
              "no finding names the missing nvm: %r" % findings)


def test_compiler_edit_alone_keeps_native_dlls_fresh():
    #The DLL input set is the native units and the headers they reach:
    #folding the full src sweep onto them would redden every tree after
    #any compiler edit and teach people to ignore the preflight. The
    #tools keep the full sweep -- they compile the whole product.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(repo / "src" / "vm" / "vm_core.cpp", 900)
        findings = run_check(repo, ["%s:Debug" % tree])
        check([f for f in findings if "nlang_io.dll" in f] == [],
              "a compiler edit must not stale the DLLs: %r" % findings)
        check(any("ncc.exe" in f for f in findings),
              "the tools must still carry the full sweep: %r" % findings)


def test_stale_native_dll_is_flagged():
    #abi.h is reached from the native unit through its <nlang/...> path.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(tree / "native" / "Debug" / "nlang_io.dll", 100)
        set_mtime(repo / "include" / "nlang" / "abi.h", 300)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("nlang_io.dll" in f and "abi.h" in f for f in findings),
              "no finding names the DLL and its input: %r" % findings)


def test_unreached_public_header_edit_keeps_native_dlls_fresh():
    #A public header no native unit includes is not a native input. The
    #2026-11-24 session paid for judging all of include/: one compiler
    #header (BuildEnvironment.h) staled seven DLLs in every tree, and
    #no build refreshes a DLL for a header it does not depend on. The
    #tools compile that header and still carry the finding.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(repo / UNREACHED_HEADER, 900)
        findings = run_check(repo, ["%s:Debug" % tree])
        check([f for f in findings if "nlang_io.dll" in f] == [],
              "a header native code never includes must not stale the "
              "DLLs: %r" % findings)
        check(any("ncc.exe" in f and "ModuleBuilder.h" in f
                  for f in findings),
              "the tools must still carry the finding: %r" % findings)


def test_header_reached_through_a_relative_include_stales_the_dll():
    #A native unit compiles every header it includes, wherever the file
    #lives: a helper outside src/native reached through a relative path
    #is a native input like the unit's own sources.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(repo / "src" / "runtime" / "NativeTimers.h", 900)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("nlang_io.dll" in f and "NativeTimers.h" in f
                  for f in findings),
              "no finding names the DLL and the reached helper: %r"
              % findings)


def test_same_dir_native_header_stales_the_dll():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(repo / "src" / "native" / "io" / "io_slots.h", 900)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("nlang_io.dll" in f and "io_slots.h" in f
                  for f in findings),
              "no finding names the DLL and its private header: %r"
              % findings)


def test_real_native_inputs_follow_the_include_lines():
    #The walker must resolve the real tree's conventions, not only the
    #stub's: the native modules reach the VM solely through the host
    #callback header (src/native/CMakeLists.txt), and no compiler header
    #is among their inputs.
    inputs = [Path(path).relative_to(REPO).as_posix()
              for path in gate_freshness.native_inputs(REPO)]
    for header in ("include/nlang/vm/NativeHost.h",
                   "include/nlang/runtime/StackDump.h"):
        check(header in inputs,
              "the native inputs must reach %s: %r" % (header, inputs))
    compiler_headers = [path for path in inputs
                        if path.startswith("include/nlang/compiler/")]
    check(compiler_headers == [],
          "a compiler header is not a native input: %r" % compiler_headers)


def test_real_native_units_have_no_unresolved_project_include():
    #An include the walker cannot resolve is judged as a system header
    #and leaves the rule: the convention is total today except the
    #vendored zlib header, and a new unresolved quoted include must fail
    #here rather than silently drop an input.
    _, unresolved = gate_freshness.include_closure(
        REPO, gate_freshness.native_sources(REPO))
    check(set(unresolved) <= VENDORED_NATIVE_INCLUDES,
          "quoted includes the native rule cannot resolve: %r" % unresolved)


def test_missing_stdlib_copy_is_flagged():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo = make_repo(tmp / "repo")
        tree = make_tree(tmp / "tree", stdlib=False)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("util.n" in f for f in findings),
              "no finding names the missing copy: %r" % findings)


def test_stale_stdlib_copy_is_flagged():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(tree / "stdlib" / "util.n", 100)
        set_mtime(repo / "stdlib" / "util.n", 150)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("util.n" in f for f in findings),
              "no finding names the stale copy: %r" % findings)


def test_stdlib_edit_alone_keeps_tools_fresh():
    #The package rule carries stdlib (the runner's documented lesson:
    #stdlib is not a tool input -- mixing it in pressured no-op tool
    #rebuilds and once invited a timestamp-forging touch).
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(repo / "stdlib" / "util.n", 500)
        findings = run_check(repo, ["%s:Debug" % tree])
        check([f for f in findings
               if "ncc.exe" in f or "nlang_io.dll" in f] == [],
              "a stdlib edit must not stale the tools: %r" % findings)


def test_version_header_newer_than_artifact_is_flagged():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(tree / "generated" / "nlang_version.h", 900)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("nlang_version.h" in f for f in findings),
              "no finding names the version header: %r" % findings)


def test_ctest_tree_checks_test_executables():
    #Test binaries are presence-only by design: their per-target input
    #graph is the build system's, and judging them by any repo-wide
    #sweep gives unactionable false positives (the rebuild the finding
    #demands is the one the build system just judged unnecessary).
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo = make_repo(tmp / "repo")
        tree = make_tree(tmp / "tree", tests=True)
        write(repo / "tests" / "test_vm.cpp")
        set_mtime(repo / "tests" / "test_vm.cpp", 800)
        findings = run_check(
            repo, [gate_freshness.TreeSpec(str(tree), "Debug", True)])
        check(findings == [],
              "presence-only must not flag built test binaries: %r"
              % findings)
        #A plain --tree never even requires them to exist.
        plain = run_check(repo, [gate_freshness.TreeSpec(str(tree),
                                                         "Debug", False)])
        check(plain == [], "plain trees judge no test binaries: %r" % plain)


def test_ctest_tree_without_test_binaries_is_flagged():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo = make_repo(tmp / "repo")
        tree = make_tree(tmp / "tree", tests=False)
        findings = run_check(
            repo, [gate_freshness.TreeSpec(str(tree), "Debug", True)])
        check(any("test" in f for f in findings),
              "no finding names the missing test binaries: %r" % findings)


def test_malformed_spec_is_a_usage_error():
    expect_usage_error(
        lambda: gate_freshness.TreeSpec.parse("no-separator-here"),
        "a spec without a config separator")


def test_stale_stdlib_package_vs_source_is_flagged():
    #The compiled package is judged the way the e2e runner judges it:
    #older than the newest stdlib source means the edit missed the
    #package (a full sweep ran green against a stale one on 2026-10-31).
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        write_package(tree, mtime=50)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("io.npkg" in f and "util.n" in f for f in findings),
              "no finding names the package and its source: %r" % findings)


def test_stdlib_package_older_than_ncc_is_flagged():
    #A package older than the ncc beside it was packed by a superseded
    #compiler -- the runner's rule verbatim, hoisted to the preflight.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        write_package(tree, mtime=150)
        set_mtime(tree / "src" / "tools" / "ncc" / "Debug" / "ncc.exe", 300)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("io.npkg" in f and "ncc.exe" in f for f in findings),
              "no finding names the package and the ncc: %r" % findings)


def test_fresh_stdlib_package_is_not_flagged():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        write_package(tree, mtime=400)
        findings = run_check(repo, ["%s:Debug" % tree])
        check([f for f in findings if ".npkg" in f] == [],
              "a fresh package must not be flagged: %r" % findings)


def test_missing_stdlib_package_is_flagged():
    #The tree's generated projects name its package set: a project
    #without its package means the packing target has not run since the
    #configure, and every import of that domain would fail at load time.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        write_package_project(tree)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("MISSING" in f and "io.npkg" in f for f in findings),
              "no finding names the missing package: %r" % findings)


def test_one_stale_domain_package_is_flagged_on_its_own():
    #The packages are always rebuilt together, but each is judged on
    #its own stamp: a fresh one must not cover for a stale sibling.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        write_package(tree, "io", mtime=400)
        write_package(tree, "web", mtime=50)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("web.npkg" in f and "util.n" in f for f in findings),
              "no finding names the stale package: %r" % findings)
        check([f for f in findings if "io.npkg" in f] == [],
              "the fresh package must not be flagged: %r" % findings)


def test_submodule_stdlib_copy_keeps_its_relative_path():
    #A dotted sub-module (stdlib/web/static.n = module "web.static")
    #lives under its unit's directory; the tree's copy is judged at the
    #same relative path, not flattened next to the top-level units.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(write(repo / "stdlib" / "web" / "static.n"), 150)
        findings = run_check(repo, ["%s:Debug" % tree])
        missing = [f for f in findings
                   if "MISSING" in f and "static.n" in f
                   and "web" in f]
        check(missing != [],
              "no finding names the missing sub-module copy: %r" % findings)


def test_submodule_stdlib_edit_stales_the_packages():
    #A sub-module source is a package input like any other unit: the
    #newest stdlib source bounds the packages with it included.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(write(repo / "stdlib" / "web" / "static.n"), 500)
        write_package(tree, mtime=250)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("io.npkg" in f and "static.n" in f for f in findings),
              "no finding names the package and the sub-module: %r"
              % findings)


def test_version_file_binds_tools_and_native_dlls():
    #A VERSION bump rides the version/ABI the tools and DLLs embed and
    #needs no source edit to be real; an artifact older than the repo's
    #VERSION file predates the bump's build (recorded ruling: a bump
    #rebuilds every config the legs run).
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(write(repo / "VERSION"), 900)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("ncc.exe" in f and "VERSION" in f for f in findings),
              "no finding binds the tools to VERSION: %r" % findings)
        check(any("nlang_io.dll" in f and "VERSION" in f for f in findings),
              "no finding binds the DLLs to VERSION: %r" % findings)


def test_empty_input_sweep_is_a_usage_error():
    #An empty sweep can never judge anything stale: refusing is the
    #only honest verdict (the tier assembly reds on empty selections
    #for the same reason).
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo = tmp / "repo"
        (repo / "stdlib").mkdir(parents=True)
        write(repo / "stdlib" / "util.n")
        expect_usage_error(
            lambda: gate_freshness.check(
                repo, [gate_freshness.TreeSpec(str(tmp / "tree"),
                                               "Debug", False)]),
            "an empty src/include sweep")


def test_empty_stdlib_sweep_is_a_usage_error():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo = tmp / "repo"
        for rel in REPO_INPUTS[:2]:  #src and include, no stdlib module
            write(repo / rel)
        expect_usage_error(
            lambda: gate_freshness.check(
                repo, [gate_freshness.TreeSpec(str(tmp / "tree"),
                                               "Debug", False)]),
            "an empty stdlib sweep")


def test_repo_without_native_units_is_a_usage_error():
    #Without the units the native rule has nothing to judge and the DLLs
    #would pass on the VERSION bound alone: refusing is the only honest
    #verdict, as for the other empty sweeps.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo = tmp / "repo"
        for rel in REPO_INPUTS:
            write(repo / rel)
        expect_usage_error(
            lambda: gate_freshness.check(
                repo, [gate_freshness.TreeSpec(str(tmp / "tree"),
                                               "Debug", False)]),
            "a repo without native units")


def test_required_tool_needs_the_exact_binary():
    #A decoy inside the tool's own directory (ncc-old.exe) must not
    #satisfy the required ncc.exe; neither may an unrelated path
    #segment that merely spells "/ncc/".
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        (tree / "src" / "tools" / "ncc" / "Debug" / "ncc.exe").unlink()
        set_mtime(write(tree / "src" / "tools" / "ncc" / "Debug" /
                        "ncc-old.exe"), 200)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("MISSING ncc" in f for f in findings),
              "the decoy must not satisfy the required ncc: %r" % findings)


def test_vanished_artifact_is_a_clean_error():
    #A file vanishing between glob and getmtime is a red, but the
    #operator should read a finding, not a traceback.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        real_getmtime = os.path.getmtime

        def flaky_getmtime(path):
            if "nvm.exe" in str(path):
                raise FileNotFoundError(path)
            return real_getmtime(path)

        os.path.getmtime = flaky_getmtime
        try:
            captured = io.StringIO()
            with redirect_stderr(captured):
                rc = gate_freshness.main(
                    ["check", "--repo", str(repo),
                     "--tree", "%s:Debug" % tree])
        finally:
            os.path.getmtime = real_getmtime
        check(rc == 2, "a vanished artifact must exit 2, got %s" % rc)
        check("disappeared" in captured.getvalue(),
              "the error must say what happened: %r" % captured.getvalue())


TESTS = [
    test_fresh_tree_passes,
    test_stale_tool_exe_is_flagged_with_the_input_named,
    test_missing_tool_is_flagged,
    test_compiler_edit_alone_keeps_native_dlls_fresh,
    test_stale_native_dll_is_flagged,
    test_unreached_public_header_edit_keeps_native_dlls_fresh,
    test_header_reached_through_a_relative_include_stales_the_dll,
    test_same_dir_native_header_stales_the_dll,
    test_real_native_inputs_follow_the_include_lines,
    test_real_native_units_have_no_unresolved_project_include,
    test_missing_stdlib_copy_is_flagged,
    test_stale_stdlib_copy_is_flagged,
    test_stdlib_edit_alone_keeps_tools_fresh,
    test_version_header_newer_than_artifact_is_flagged,
    test_ctest_tree_checks_test_executables,
    test_ctest_tree_without_test_binaries_is_flagged,
    test_malformed_spec_is_a_usage_error,
    test_stale_stdlib_package_vs_source_is_flagged,
    test_stdlib_package_older_than_ncc_is_flagged,
    test_fresh_stdlib_package_is_not_flagged,
    test_missing_stdlib_package_is_flagged,
    test_one_stale_domain_package_is_flagged_on_its_own,
    test_submodule_stdlib_copy_keeps_its_relative_path,
    test_submodule_stdlib_edit_stales_the_packages,
    test_version_file_binds_tools_and_native_dlls,
    test_empty_input_sweep_is_a_usage_error,
    test_empty_stdlib_sweep_is_a_usage_error,
    test_repo_without_native_units_is_a_usage_error,
    test_required_tool_needs_the_exact_binary,
    test_vanished_artifact_is_a_clean_error,
]


def main():
    failures = 0
    for test in TESTS:
        try:
            test()
        except AssertionError as err:
            failures += 1
            print("FAIL %s: %s" % (test.__name__, err))
        except Exception as err:  #an unexpected crash is a failure too
            failures += 1
            print("ERROR %s: %r" % (test.__name__, err))
    verdict = "PASS" if failures == 0 else "FAIL"
    print("GATE gate_freshness_selftest: %s (%d tests, %d failed)"
          % (verdict, len(TESTS), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
