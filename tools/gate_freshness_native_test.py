"""gate_freshness_native_test.py -- the native-DLL rule's cases of the
freshness self-test corpus. The rule judges the units under src/native
plus the files they reach through #include lines, by one bound for the
whole class. A stub repo proves each resolution form (a public header
through its <nlang/...> path, a same-directory private header, a helper
outside src/native through a relative path) and the exclusion that
motivated the rule (a public header no native unit includes, which the
tools compile and the DLLs do not); two cases read the real tree to
prove the walker resolves the project's own include conventions. Entry
point: gate_freshness_test.py (its TESTS list names these cases).
Stdlib only.
"""
import tempfile
from pathlib import Path

import gate_freshness
from gate_freshness_testutil import (
    REPO, REPO_INPUTS, UNREACHED_HEADER, VENDORED_NATIVE_INCLUDES, check,
    expect_usage_error, make_repo, make_tree, run_check, set_mtime, write)


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
