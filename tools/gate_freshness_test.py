"""gate_freshness_test.py -- self-test corpus for the batch freshness
preflight (tools/gate_freshness.py): the per-class input rules (tools
carry the full src/include sweep; test executables presence-only; the
native DLL rule's cases live in gate_freshness_native_test.py), the
stdlib package-copy rule (dotted sub-modules by relative path), the
per-domain stdlib package rules (a generated <domain>.nproj must have
its package, and the package must be as new as source and ncc), the
VERSION-file bound, the empty-sweep refusals, the exact-filename
required-tool check and the tree-spec parsing. Stub repos and trees
with pinned mtimes (gate_freshness_testutil.py) keep the corpus
hermetic: no third-party imports, no build, no network. Run either
way:
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
from gate_freshness_native_test import (  # noqa: E402
    test_compiler_edit_alone_keeps_native_dlls_fresh,
    test_header_reached_through_a_relative_include_stales_the_dll,
    test_real_native_inputs_follow_the_include_lines,
    test_real_native_units_have_no_unresolved_project_include,
    test_repo_without_native_units_is_a_usage_error,
    test_same_dir_native_header_stales_the_dll,
    test_stale_native_dll_is_flagged,
    test_unreached_public_header_edit_keeps_native_dlls_fresh)
from gate_freshness_testutil import (  # noqa: E402
    REPO_INPUTS, check, expect_usage_error, make_repo, make_tree,
    run_check, set_mtime, write, write_package, write_package_project)


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


def test_version_file_binds_tools_but_not_native_dlls():
    #A VERSION bump rides the version string the tools' main units
    #embed (NLANG_VERSION, from the generated header) and needs no
    #source edit to be real; an artifact older than the repo's VERSION
    #file predates the bump's build (recorded ruling: a bump rebuilds
    #every config the legs run). The native DLLs bind to their own
    #unit closure instead: they embed the host ABI handshake
    #(NLANG_HOST_ABI_VERSION in nlang/vm/NativeHost.h), which is part
    #of that closure and still staleness-checked there, but no native
    #unit includes the generated version header, so binding the DLLs
    #to VERSION demanded a rebuild the build system had already judged
    #unnecessary -- every bump re-ran the finding (14 STALE lines on
    #the merged 0.13.13 trees, an unactionable false positive).
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(write(repo / "VERSION"), 900)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("ncc.exe" in f and "VERSION" in f for f in findings),
              "no finding binds the tools to VERSION: %r" % findings)
        check([f for f in findings if "nlang_io.dll" in f] == [],
              "VERSION must not stale the native DLLs: %r" % findings)


def test_version_bump_stales_native_dll_through_abi_header_only():
    #The DLLs' version channel is the ABI handshake, and it is guarded
    #through the closure: an edit to NativeHost.h (the header that
    #defines NLANG_HOST_ABI_VERSION) must stale the DLLs on its own,
    #with no VERSION file involved.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        set_mtime(repo / "include" / "nlang" / "abi.h", 900)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("nlang_io.dll" in f and "abi.h" in f for f in findings),
              "the ABI header edit must stale the DLLs: %r" % findings)


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


def test_deployed_qt_helper_is_not_judged_against_repo_sources():
    #The Qt WebEngine helper process deployed beside nide is runtime
    #data from the Qt installation: copy_if_different never refreshes
    #its mtime for content that cannot change with a repo edit, so the
    #sweep's demanded rebuild is unactionable -- the same reasoning
    #that makes test executables presence-only. Both spellings (the
    #Debug trailing d and the Release bare name) ride the exclusion.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        nide = tree / "src" / "tools" / "nide" / "Debug"
        set_mtime(write(nide / "nide.exe"), 200)
        set_mtime(write(nide / "QtWebEngineProcessd.exe"), 50)
        set_mtime(write(nide / "QtWebEngineProcess.exe"), 50)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(not any("QtWebEngineProcess" in f for f in findings),
              "the deployed helper is Qt install data, not a product: %r"
              % findings)


def test_stale_product_nide_is_still_flagged():
    #The exclusion is name-precise: the product nide.exe deployed from
    #this repo's own sources keeps its full-sweep judgment.
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, tree = make_repo(tmp / "repo"), make_tree(tmp / "tree")
        nide = tree / "src" / "tools" / "nide" / "Debug"
        set_mtime(write(nide / "nide.exe"), 50)
        findings = run_check(repo, ["%s:Debug" % tree])
        check(any("nide.exe" in f for f in findings),
              "a stale product nide must stay flagged: %r" % findings)


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
    test_version_file_binds_tools_but_not_native_dlls,
    test_version_bump_stales_native_dll_through_abi_header_only,
    test_empty_input_sweep_is_a_usage_error,
    test_empty_stdlib_sweep_is_a_usage_error,
    test_repo_without_native_units_is_a_usage_error,
    test_required_tool_needs_the_exact_binary,
    test_deployed_qt_helper_is_not_judged_against_repo_sources,
    test_stale_product_nide_is_still_flagged,
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
