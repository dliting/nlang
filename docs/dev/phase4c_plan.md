# Phase 4c — Retire the built-in standard library (kStdLibTable + math/io/fs intrinsics)

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development
> or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** delete the last hardcoded standard-library path in the VM — the
`kStdLibTable` signature table and the three unreachable intrinsic families — so
`io`/`math`/`fs` exist only as `stdlib/*.n` sources plus `nlang_<ns>.dll`.

**Architecture:** after Phase 4a, a qualified library call compiles to
`OP_CallFunc` on a fully-qualified name and the callee runs from the native DLL;
nothing emits `INTR_Math_*` / `INTR_Io_*` / `INTR_FileSystem_*` ids any more, so
the table and 576 lines of family implementations are dead. The string-method
family (`INTR_String_*`, ids 95-106) stays: receiver-dispatched built-in methods
still run through `OP_CallIntrinsic`.

**Tech Stack:** C++17, CMake (Visual Studio 17 2022, config `Release`), hand-rolled
`TEST/CHECK/PASS` harness in `tests/test_vm/`, python guards registered as ctest.

**Spec:** `docs/dev/phase4bcd_design.md` §2 (decisions approved by the user 2026-09-28).

## Global Constraints

- **Phase gate (user rule, 2026-09-28):** a phase is finished when its audit loop
  (Task 8) reports a round with zero Critical/Important findings — not when the tests
  go green. `docs/dev/phase4d_plan.md` does not start before that verdict exists.
- No backward compatibility: retired code is deleted, not shims, not `#if` guarded,
  not renamed to `_legacy`. (CLAUDE.md: 不需要向下兼容.)
- Build principles: TDD (red → green), SOLID, real compile + real bytecode execution
  in tests, no mocks.
- `source_size_guard`: ≤500 lines per file, ≤50 per function
  (`tools/source_size_guard/check_source_size.py`, ctest `source_size_guard`).
- Public text (docs, help, messages) must carry no trace of the retired engine —
  ctest `nlang_docs_pytest` runs `tools/nlang-docs/tests/test_public_text.py`.
- Commits: Conventional Commits, English, one logical change each.
  **Ask the user before every commit** (CLAUDE.md: 提交前必须先经过用户确认). Never
  push. Never touch `master`. Never commit `temp/`, `build-dev/`, `AGENTS.md`.
- Verification is **serial**: `ctest --test-dir build-dev/tests -C Release` with no
  `-j`. `mainwindow_tests` and `nide_deploy_check` fail under parallel runs
  (shared temp dir / `NLANG_TEST_ROOT` contention), verified 2026-09-28.
- Every task ends with a serial full ctest run at 63/63 (63 becomes the count this
  plan establishes: +1 guard, −1 retired generator ⇒ still 63).
- Python: use the interpreter ctest is configured with,
  `D:/dev/miniconda3/python.exe` (`PYTHON3_EXECUTABLE` in `build-dev/CMakeCache.txt`).
  Plain `python` in this shell is the WindowsApps stub and fails with
  `Permission denied`.
- The user's uncommitted Phase 4b-2 work is **parked, not in the tree** (decided
  2026-09-28 after Task 1 landed as `714b317`): its 21 code files are byte-copied under
  `temp/wip_4b2/` (list in `temp/wip_4b2/files.txt`, verified by `temp/wip_4b2/SHA256SUMS`)
  and were then reverted to HEAD. Proof this was the right call: with those 21 files in
  the tree the suite is 51/63 (4 suites, 12 cases — `library_source_tests` 8 WIP-added
  scenarios plus regressions in `array_flags_tests`, `module_import_tests:831`,
  `array_property_tests:164,187`), and with them parked the suite is 63/63. The five files
  this plan touches — `src/compiler/builder/ModuleRegistry.{cpp,h}`, `ExprResolver.h`,
  `DuplicateFieldChecker.hpp`, `ExprResolverTypes.cpp`, `ExprResolverMember.cpp` — are
  therefore clean HEAD copies: **edit them directly, Appendix A is not needed for 4c**.
  `mkdocs.{en,zh}.yml` and the two untracked `library-mechanism.md` pages stay in the tree
  (Task 6 needs them). Never `git stash` in this worktree.

  **Restore protocol after 4c lands (audit round 1, Important 1+2).** A byte copy-back of
  the parked set does NOT compile and must not be done blindly:
  - `temp/wip_4b2/src/compiler/builder/ModuleRegistry.cpp:81` still calls
    `IsStdLibNamespaceName`, which `81fab11` deleted — the parked copy must be re-edited to
    `IsReservedLibraryName` and its `#include <nlang/vm/StdLib.h>` dropped (the same include
    is now dead in the parked `ExprResolverMember.cpp:14`, `ExprResolverTypes.cpp:11`,
    `DuplicateFieldChecker.hpp:8`, and the parked `ExprResolver.h:148-154` still
    forward-declares the deleted `StdLibEntry`). Five overlapping files:
    `ModuleRegistry.{cpp,h}`, `ExprResolver.h`, `ExprResolverMember.cpp`,
    `ExprResolverTypes.cpp`, `DuplicateFieldChecker.hpp` — restore them by Appendix A's
    Step 5 (re-apply 4c's edit inside the WIP copy), not by `cp`.
  - `temp/wip_4b2/docs_preserve/docs/{en,zh}/vm-architecture/library-mechanism.md` are the
    **pre-4c** drafts; `3bd962f` committed rewritten versions. They are kept only as the
    byte-integrity record (`DOCS_WIP_SHA256SUMS`) and must never be copied back; any 4b-2
    doc additions have to be re-written against the committed pages. The two preserved
    `mkdocs.{en,zh}.yml` are byte-identical to the committed ones, so that part of a
    restore is a no-op.
  After the merge, the 4b-2 *test* failures were expected to return (51/63 as measured at
  `da5e4a8`) — they would have been the user's in-progress feature, not a 4c regression.

  **Restore outcome (2026-09-29, executed and committed as `ded5bbc`).** The merge was done
  as written: the 15 non-overlapping parked files came back as deltas, the 5 overlapping
  ones were copied byte-for-byte from `temp/wip_4b2/` and then re-edited in place for 4c's
  deletions (deltas vs the parked copies are exactly 4c's edits — verified with
  `diff -u temp/wip_4b2/<path> <path>` per file). `docs_preserve/` was NOT copied back; the
  doc pages that 4c committed stayed authoritative and only gained the 4b-2 additions
  (§3 type-surface bullet, §8 moved 4b-2 from open to landed, both languages, plus a
  roadmap bullet).

  The 51/63 expectation above **did not hold**, and the tree is not in a surprising state:
  with 4c landed underneath, the same 21 WIP files give **63/63 serial**, `no_builtin_stdlib`
  green, and the 4 new `library_source_tests` scenarios present and passing (suite runs 15
  cases). A real `ncc`+`nvm` CLI round-trip of the library-inheritance scenario returns 0,
  with a negative control (a deliberately false branch) returning its own code, so the 0 is
  not vacuous. Reading of the two facts together: those 12 failures were the WIP colliding
  with the machinery 4c deletes — i.e. **4c is a prerequisite of 4b-2**, which is why
  parking was the right call, not because the WIP was broken on its own terms. Do not chase
  51/63 as a baseline again; the 4b-2 baseline is 63/63.

---

## Appendix A: partial staging over a WIP file

Used whenever a file contains both the user's uncommitted work and a line this plan
must change (e.g. `ModuleRegistry.cpp:81`).

**Status 2026-09-28: not used by Tasks 2-7.** The 4b-2 work was parked whole in
`temp/wip_4b2/` before 4c's table deletion began (see Global Constraints), so every file
this plan edits is a clean HEAD copy. Keep this procedure for the restore step and for any
later phase that must land on top of a live WIP file.

- [ ] **Step 1: back up the working copy byte-for-byte**

```bash
mkdir -p temp/wip_backup/$(dirname <file>) && cp -p <file> temp/wip_backup/<file>
sha256sum <file> temp/wip_backup/<file>    # both hashes must match
```

- [ ] **Step 2: take the file to HEAD and apply only this plan's edit**

```bash
git checkout -- <file>
```

Then edit the HEAD copy (Edit tool). Do **not** run the build yet.

- [ ] **Step 3: stage exactly that edit and commit it**

```bash
git add <file>
git diff --cached --stat -- <file>          # expect only the lines of this task
git commit -m "<message>"
```

- [ ] **Step 4: restore the user's work on top of the new HEAD**

```bash
cp -p temp/wip_backup/<file> <file>
```

- [ ] **Step 5: re-apply this task's edit inside the restored WIP copy**

Re-apply by hand (Edit tool). Verify: `git diff -- <file>` now shows only the 4b-2
hunks (no residual of this task's edit, because it is committed), and
`git diff HEAD~1 -- <file>` contains both. Rebuild and re-run tests before moving on.

## Task 1: Move the shared exception seam out of the math family

**Status: DONE — landed as `714b317` (2026-09-28).** Net-zero relocation verified by
`git diff --stat`; the header comment at `VmExecutor.h:606` was corrected in the same
change. Isolation check: reverting both files reproduced the same 4 failing suites, so
those failures belong to the parked 4b-2 work, not to this task.

**Files:**
- Modify: `src/vm/IntrinsicsMath.cpp:28-34` (delete `RaiseNlangExceptionBase`)
- Modify: `src/vm/IntrinsicsString.cpp` (insert after `s_TYPE_SIZE`, i.e. after line 32
  `static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes`)
- Read: `src/vm/VmExecutor.h:607` (declaration — unchanged)

**Interfaces:**
- Consumes: nothing.
- Produces: `VmExecutor::RaiseNlangExceptionBase(const std::string&)` now defined in
  `IntrinsicsString.cpp`, so deleting the math TU cannot break the surviving string TU.
  Verified users to keep working: `src/vm/IntrinsicsString.cpp:202,247,276,286`
  (the math users at `IntrinsicsMath.cpp:98,141,153,183,210` die with the file in
  Task 4).

- [ ] **Step 1: cut the definition out of `src/vm/IntrinsicsMath.cpp`**

Delete lines 28-34 verbatim:

```cpp
//Phase 11 error model: argument/range errors raise the BASE Exception.
//Defined here (first stdlib family TU) but declared in the header so the
//string/io/fs TUs reuse the same seam.
[[noreturn]] void VmExecutor::RaiseNlangExceptionBase(const std::string& msg)
{
	RaiseNlangException(m_exceptionClassIdx, msg);
}
```

- [ ] **Step 2: paste it into `src/vm/IntrinsicsString.cpp` after line 32**

Reword the comment to state where it lives now (the "first stdlib family TU"
rationale no longer holds):

```cpp
//Phase 11 error model: argument/range errors raise the BASE Exception.
//Declared in VmExecutor.h; defined here because this is the one TU that
//still raises intrinsically since the math/io/fs families retired.
[[noreturn]] void VmExecutor::RaiseNlangExceptionBase(const std::string& msg)
{
    RaiseNlangException(m_exceptionClassIdx, msg);
}
```

- [ ] **Step 3: build**

Run: `cmake --build build-dev --config Release -j 8`
Expected: exit 0, no `error C` lines. This step must be green on its own — it is a
pure move, so every test still passes.

- [ ] **Step 4: run the tests that exercise the seam**

Run: `ctest --test-dir build-dev/tests -C Release -R "stdlib_tests|string|vm_tests" --output-on-failure`
Expected: all listed tests Passed. A misplaced raise would show up as a string
method (`substring` range error, `split`) failing or crashing.

- [ ] **Step 5: commit (ask the user first)**

```bash
git add src/vm/IntrinsicsMath.cpp src/vm/IntrinsicsString.cpp
git commit -m "refactor(vm): move the intrinsic exception seam to the string family

RaiseNlangExceptionBase lived in IntrinsicsMath.cpp (first stdlib family TU)
but is shared by the string family; relocate it there so the math/io/fs
families can be deleted without taking the seam with them."
```

## Task 2: Add the static guard that the VM carries no built-in stdlib

**Status: DONE, deliberately left RED (2026-09-28).** `tests/check_no_builtin_stdlib.py`
(67 lines, complete) + the `no_builtin_stdlib` ctest block at `tests/CMakeLists.txt:890-895`
are in the working tree, uncommitted. Guard prints 35 problems / exit 1; clauses 3 and 4
pass (math 25, io 5, fs 8 native decls; `ExecuteIntrinsicString` still wired). Suite total
is now **64** (`ctest -N`). Task 3 lands the guard together with the table deletion.

**Files:**
- Create: `tests/check_no_builtin_stdlib.py`
- Modify: `tests/CMakeLists.txt` (register next to the other python guards,
  after the block at lines 884-889 that currently holds `stdlib_generation`)
- Test: the script itself is the test

**Interfaces:**
- Consumes: nothing.
- Produces: ctest `no_builtin_stdlib` (exit 0 = retired, exit 1 = something came
  back). Task 3 and Task 4 turn its clauses green one family at a time; the pinned
  native counts (25/5/8, verified from `stdlib/math.n`, `stdlib/io.n`,
  `stdlib/fs.n` on 2026-09-28) stay true through the whole plan.

- [ ] **Step 1: write the guard**

```python
#!/usr/bin/env python3
"""Phase 4c guard: the VM must not carry a built-in standard library.

io/math/fs are library .n sources compiled by the compiler whose native
functions are served by nlang_<ns>.dll through the NativeHost ABI. The
hardcoded path that preceded that (kStdLibTable + the ExecuteIntrinsicMath /
Io / Fs families + their intrinsic ids) is retired; this guard fails if any
piece of it returns, and fails if the library sources it depends on are
deleted instead of the table.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


# 1. Retired symbols and ids must be gone from all C++ sources.
BANNED = [
    r"\bkStdLibTable\b", r"\bStdLibEntry\b", r"\bFindStdLibFunction\b",
    r"\bIsStdLibNamespaceName\b",
    r"\bExecuteIntrinsicMath\b", r"\bExecuteIntrinsicIo\b",
    r"\bExecuteIntrinsicFs\b",
    r"\bINTR_Math_", r"\bINTR_Io_", r"\bINTR_FileSystem_",
    r"\bkMathIntrinsic", r"\bkIoIntrinsic", r"\bkFileSystemIntrinsic",
]
for sub in ("src", "include"):
    for path in sorted((ROOT / sub).rglob("*")):
        if not path.is_file() or path.suffix not in (
                ".h", ".hpp", ".cpp", ".cxx", ".y", ".l"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in BANNED:
            check(not re.search(pattern, text),
                  f"{path.relative_to(ROOT)}: retired symbol {pattern} present")

# 2. The three family TUs must not exist (the string family and the file
# stream family are different code paths and stay).
for gone in ("src/vm/IntrinsicsMath.cpp", "src/vm/IntrinsicsIo.cpp",
             "src/vm/IntrinsicsFs.cpp"):
    check(not (ROOT / gone).exists(), f"{gone} must be deleted")

# 3. Positive control: the mechanism the deletion leans on is still there.
NATIVE_DECL = re.compile(r"^\s*native\s+\S", re.MULTILINE)
for ns, expected in (("math", 25), ("io", 5), ("fs", 8)):
    source = (ROOT / "stdlib" / f"{ns}.n").read_text(encoding="utf-8")
    found = len(NATIVE_DECL.findall(source))
    check(found == expected,
          f"stdlib/{ns}.n must declare {expected} native functions, found {found}")

# 4. The surviving intrinsic path must survive: string methods still dispatch.
backend = (ROOT / "src/vm/VmExecutorIntrinsics.cpp").read_text(encoding="utf-8")
check("ExecuteIntrinsicString" in backend,
      "the string-method family dispatch must remain wired")

if failures:
    for f in failures:
        print("FAIL: " + f)
    print(f"{len(failures)} problem(s): the built-in standard library is not fully retired")
    sys.exit(1)
print("no builtin stdlib: table, families and ids are retired; stdlib sources intact")
```

- [ ] **Step 2: run it and read the red**

Run: `D:/dev/miniconda3/python.exe tests/check_no_builtin_stdlib.py`
Expected: **FAIL**, with `kStdLibTable`, `StdLibEntry`, `IsStdLibNamespaceName`,
`INTR_Math_`, `INTR_Io_`, `INTR_FileSystem_`, `ExecuteIntrinsicMath/Io/Fs` and the
three existing files all listed — clauses 3 and 4 already pass. If it reports
`ImportError`/`Permission denied` for python, stop and fix the interpreter first
(the same interpreter must work under ctest).

- [ ] **Step 3: register it as a ctest**

Insert directly after the `stdlib_generation` block
(`tests/CMakeLists.txt:884-889`):

```cmake
    # Phase 4c guard: no hardcoded standard library may return to the VM
    # (signature table, math/io/fs intrinsic families, their id blocks).
    add_test(NAME no_builtin_stdlib
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_no_builtin_stdlib.py)
    set_tests_properties(no_builtin_stdlib PROPERTIES TIMEOUT 60)
```

- [ ] **Step 4: reconfigure and confirm the test runs (and fails)**

```bash
cmake -S . -B build-dev
ctest --test-dir build-dev/tests -C Release -R no_builtin_stdlib --output-on-failure
```
Expected: `no_builtin_stdlib ... Failed` — a red test wired into the suite.

- [ ] **Step 5: do not commit yet**

A red test must not be committed alone; Task 3 completes the cycle. (If the
user wants the guard committed with the table deletion, that is exactly what
Task 3's commit does.)

## Task 3: Delete the signature table, relocate the reserved library names

**Files:**
- Modify: `include/nlang/vm/StdLib.h` — delete `:35-49` (the `//ABI note:` comment that
  describes namespace intrinsics + `StdLibEntry`), `:51-57` (`IsStdLibNamespaceName` with
  its comment), `:59-110` (`kStdLibTable` with its comment), `:112-198` (the three
  table↔id binding functions and their `static_assert` blocks), `:286-299`
  (`FindStdLibFunction` with its comment), and rewrite the file header `:1-13`.
  Ranges re-verified against the file on 2026-09-28 (301 lines total); each deletion
  takes the explanatory comment with it, so no orphan comment survives.
- Keep in that file: `:26-33` `StdLibReturnType` (`SLRT_*`, still cast at
  `tests/test_vm/test_stdlib.cpp:396`), `:210-284`
  (`StringTrailingDefault`, `StringMethodEntry`, `kStringMethodTable`,
  `StringMethodIdsInBlock`/`StringMethodEntryCount` asserts, `FindStringMethod`)
- Modify: `include/nlang/vm/CompiledModule.h:208-210` (comment naming
  `kStdLibTable`)
- Modify: `src/compiler/builder/ModuleRegistry.cpp:68-87` — own the
  reserved names
- Modify: `src/compiler/builder/ExprResolver.h:154` — drop
  `struct StdLibEntry;`
- Modify: `src/compiler/builder/ExprResolver.cpp:88` — comment naming `StdLibEntry`
- Modify: drop the now-unneeded `#include <nlang/vm/StdLib.h>` from every compiler
  file that references no surviving symbol (list produced in Step 5)
- Delete: `tests/check_stdlib_generation.py` (136 lines) + its `stdlib_generation`
  ctest block (`tests/CMakeLists.txt:884-889`) — **folded in from Task 5**: the script
  parses `kStdLibTable[] =` out of `StdLib.h`, so it goes red the moment the table goes.
  Leaving it for a later task would ship a red commit.
- Modify: `src/compiler/builder/ExprResolverStdLib.cpp:2` — file header still names the
  Phase 4a-removed `TryResolveStdLibCall`

**Interfaces:**
- Consumes: Task 2's `no_builtin_stdlib`.
- Produces: `nlang::IsReservedLibraryName(std::string_view)` declared in
  `src/compiler/builder/ModuleRegistry.h` (compiler-side: it guards project module
  directories, not the VM). Nothing else gains a new symbol.

- [ ] **Step 1: write the reserved-name home first (red for its own sake)**

Append to the declarations in `src/compiler/builder/ModuleRegistry.h`:

```cpp
//Library namespaces that are permanently taken by stdlib/*.n + nlang_<ns>.dll:
//a project directory or module with one of these names would shadow a
//standard library. The list lives here (the compiler's gate), not in the VM.
bool IsReservedLibraryName(const std::string& name);
```

In `src/compiler/builder/ModuleRegistry.cpp`, replace the body of
`FindReservedSegment` (lines 68-87) so it calls the new predicate, and add:

```cpp
bool IsReservedLibraryName(const std::string& name)
{
	//io/math/fs each own a stdlib/<ns>.n source and an nlang_<ns>.dll.
	static const char* const kReservedLibraryNames[] = { "io", "math", "fs" };
	for (const char* reserved : kReservedLibraryNames)
		if (name == reserved)
			return true;
	return false;
}
```

and change line 81 from `if (IsStdLibNamespaceName(segment))` to
`if (IsReservedLibraryName(segment))`. Verified on the parked-WIP HEAD copy
(2026-09-28): the function is at `:72-87`, the predicate call at `:81`.

- [ ] **Step 2: prove the reserved-name gate still bites**

Run: `ctest --test-dir build-dev/tests -C Release -R "proj_reserved_segment" --output-on-failure`
Expected: `proj_reserved_segment_compile ... Passed` (it exists today and must stay
green across the move; if it were red before Step 1, the move is untested — stop).

- [ ] **Step 3: delete the table**

In `include/nlang/vm/StdLib.h` delete the ranges listed under **Files**, and replace
the header block (`:1-13`) with:

```cpp
/*---
StdLib.h — the string-method intrinsic table.

Maps a string method name (substring, split, ...) to its intrinsic id. The
SIGNATURES live in stdlib/*.n declarations and reach codegen through
langservice::SymbolIndex. Everything else the standard library used to
hardcode here (kStdLibTable for math/io/fs) is gone: those namespaces are
ordinary library sources whose native members are served by nlang_<ns>.dll
through the NativeHost ABI, exactly like a third-party package.
Header-only constexpr data: consumers live in libraries with a one-way
link (nlang_vm PRIVATE-links nlang_compiler), so a .cpp home on either
side would force a circular link.
---*/
```

- [ ] **Step 4: clear the remaining symbol references**

```bash
grep -rn "StdLibEntry\|kStdLibTable\|FindStdLibFunction\|IsStdLibNamespaceName" src include tests
```
Expected after the edits: no hits except, temporarily, `tests/check_stdlib_generation.py`
(retired in Task 5). Delete `struct StdLibEntry;` at `ExprResolver.h:154` and fix the
`ExprResolver.cpp:88` comment to name the library declaration index instead. Neither file
carries WIP content any more, so edit them in place.

- [ ] **Step 5: trim the dead includes**

```bash
for f in $(grep -rl '#include.*vm/StdLib.h' src include tests); do
  n=$(grep -cE "StringMethod|StdLibReturnType|SLRT_|STD_ReceiverLength" "$f")
  echo "$n $f"
done | sort -n
```
Expected: the files with count `0` are exactly the ones to edit — drop their
`#include <nlang/vm/StdLib.h>` line. The survivors must stay and are, verified
2026-09-28: `src/compiler/builder/ExprResolverMemberBuiltins.cpp`,
`src/vm/VmBackend.h`, `src/vm/IntrinsicsString.cpp` (via the backend header),
`tests/test_vm/test_stdlib.cpp`. (No file here is in the WIP set — it is parked in
`temp/wip_4b2/`.)

- [ ] **Step 6: retire the signature-generation checker (folded in from Task 5)**

First confirm the checker's job is genuinely covered elsewhere, so the deletion is not
a coverage hole: `ctest --test-dir build-dev/tests -C Release -R "stdlib|native|thirdparty" --output-on-failure`
→ all Passed.

Then delete the block at `tests/CMakeLists.txt:884-889` — the whole thing, comment
included:

```cmake
    # stdlib/*.n declaration guard: files generated from kStdLibTable must
    # exist and carry matching signatures (return type + param types).
    add_test(NAME stdlib_generation
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_stdlib_generation.py)
    set_tests_properties(stdlib_generation PROPERTIES TIMEOUT 60)
```

and remove the script with `git rm tests/check_stdlib_generation.py`.

Finally clear the last stale reference: `src/compiler/builder/ExprResolverStdLib.cpp:2`
names `TryResolveStdLibCall`, a function Phase 4a removed. Replace that header line with
what the file does now:

```
    ExprResolverStdLib.cpp — 库命名空间调用与跨模块限定名解析（TryResolveModuleQualified / 模块提示）。
```

Check: `grep -rn "stdlib_generation\|check_stdlib_generation\|TryResolveStdLibCall" src include tests tools` → no hits.

- [ ] **Step 7: build**

Run: `cmake --build build-dev --config Release -j 8`
Expected: exit 0. An unexpected error here means a file still needs the header —
add it back only where the symbol is really used.

- [ ] **Step 8: run the guard, then the full suite serially**

```bash
cmake -S . -B build-dev
ctest --test-dir build-dev/tests -C Release -R no_builtin_stdlib --output-on-failure
ctest --test-dir build-dev/tests -C Release
```
Expected: the guard now fails only on the *family/intrinsic* clauses (Task 4's scope) —
no `kStdLibTable` / `StdLibEntry` / `IsStdLibNamespaceName` / `FindStdLibFunction` lines
left in its output. Suite total is **63** again (64 after Task 2, −1 retired generator),
of which exactly one is red: `no_builtin_stdlib`. Do not proceed while any other test
is red, and do not "fix" the guard by deleting clauses.

- [ ] **Step 9: commit (ask the user first)**

Stage exactly this task's files — the guard script + its `tests/CMakeLists.txt` blocks
(both the added `no_builtin_stdlib` and the removed `stdlib_generation`), `StdLib.h`,
`CompiledModule.h`, `ExprResolver.{h,cpp}`, `ModuleRegistry.{h,cpp}`, the trimmed
includes, `ExprResolverStdLib.cpp` and the `git rm`'d checker. Never `git add -A`: the
working tree also holds doc changes. Nothing else in the suite may be red at this point.

```bash
git commit -m "refactor(vm): delete the hardcoded stdlib signature table

kStdLibTable mapped namespace-qualified names to intrinsic ids for codegen.
Since library .n sources are compiled inline (phase 4a) and native members
run from nlang_<ns>.dll, nothing emits or reads those ids: the table, its
static_assert blocks and FindStdLibFunction had zero callers in src/.

- StdLib.h keeps only the string-method table (receiver-dispatched built-in
  methods still run through OP_CallIntrinsic)
- the reserved library names move to the compiler gate that uses them
  (IsReservedLibraryName), out of a VM header that no longer owns them
- test: check_no_builtin_stdlib.py refuses the table, the families and their
  ids back, and pins the native declaration counts it replaced
- test: check_stdlib_generation.py is retired in the same change — it parsed
  kStdLibTable to cross-check stdlib/*.n, and the sources are now the only
  signature authority"
```

## Task 4: Retire the math/io/fs intrinsic families and their ids

**Files:**
- Delete: `src/vm/IntrinsicsMath.cpp` (215 lines after Task 1), `src/vm/IntrinsicsIo.cpp`
  (145), `src/vm/IntrinsicsFs.cpp` (208) — 568 lines total
- Modify: `src/vm/CMakeLists.txt:19,20,24`
- Modify: `src/vm/VmExecutorIntrinsics.cpp:418-428` (dispatch chain)
- Modify: `src/vm/VmExecutor.h:311-321` (four declarations, three to drop)
- Modify: `include/nlang/vm/CompiledModule.h` — delete `:183-217` (math ids +
  `kMathIntrinsic*` + asserts), `:221-232` (io), `:261-275` (fs), and the
  id-allocation comment `:175-182`
- Modify: `tools/source_size_guard/allowlist.json:62-76` (the `ExecuteIntrinsicFs`,
  `ExecuteIntrinsicIo`, `ExecuteIntrinsicMath` entries)
- Modify: `tests/test_vm/test_stdlib.cpp:279-280` (assertion basis)

**Interfaces:**
- Consumes: Task 1 (seam already relocated), Task 3 (table already gone).
- Produces: `VmExecutor` with exactly two intrinsic entry points left —
  `ExecuteIntrinsicString` and `ExecuteIntrinsicFileStream` — and the id space
  `70-94 / 110-114 / 120-127` no longer allocated.

- [ ] **Step 1: extend the guard to the family clauses (already covered)**

`tests/check_no_builtin_stdlib.py` clauses 1-2 already ban
`ExecuteIntrinsicMath/Io/Fs`, `INTR_Math_`, `INTR_Io_`, `INTR_FileSystem_`,
`kMathIntrinsic*`, `kIoIntrinsic*`, `kFileSystemIntrinsic*` and the three file
paths. Run it and record which of those still fail:
`D:/dev/miniconda3/python.exe tests/check_no_builtin_stdlib.py`
Expected: only family/intrinsic failures remain.

- [ ] **Step 2: rewrite the dispatch chain**

In `src/vm/VmExecutorIntrinsics.cpp` the chain is `:418-428` (3 comment lines + the four
family arms). **The `throw` at `:430` and the blank at `:429` stay where they are** —
replacing `418-428` with text that also contains a `throw` would emit two. Replace those
lines with the single surviving family:

```cpp
    //String methods (receiver at callParamBase slot 0, args from slot 1).
    //Chained before the unknown-id throw so each family TU stays
    //independently extensible. math/io/fs are not here: they are library
    //sources whose native members run from nlang_<ns>.dll.
    if (ExecuteIntrinsicString(intrinsicId, callParamBase, locals, pResult))
        return;
```

- [ ] **Step 3: drop the declarations and the family files**

In `src/vm/VmExecutor.h`, delete the `ExecuteIntrinsicMath`, `ExecuteIntrinsicIo` and
`ExecuteIntrinsicFs` declarations (keep `...String`) — and rewrite the comment block
above them (`:307-311`), which describes a four-family chain "see StdLib.h" and names
`IntrinsicsMath.cpp`: with one family left there is no chain. Replace it with a note that
names what is actually there (the receiver-dispatched string family, whose table lives in
`vm/StdLib.h`), and keep the existing receiver-ABI comment over `ExecuteIntrinsicString`
(`:317-319`). A comment that survives by describing a deleted mechanism is a defect.
Then:

```bash
git rm src/vm/IntrinsicsMath.cpp src/vm/IntrinsicsIo.cpp src/vm/IntrinsicsFs.cpp
```

In `src/vm/CMakeLists.txt` delete the three source lines (`19`, `20`, `24`);
`IntrinsicsString.cpp` (`21`) and `IntrinsicsFileStream.cpp` stay.

- [ ] **Step 4: delete the id blocks**

In `include/nlang/vm/CompiledModule.h` delete the three blocks **including their leading
comments**: `:183-217` (math ids, `kMathIntrinsicFirst/Count`, both asserts), `:219-232`
(io comment + ids + `kIoIntrinsic*` + asserts), `:258-275` (fs comment + ids +
`kFileSystemIntrinsic*` + asserts). Keep the string-method block `:234-256` (its comment,
`INTR_String_Substring` … `INTR_String_ToFloat`, `kStringMethodIntrinsicFirst/Count` and
its asserts). The allocation-map comment `:175-182` also goes, but it is the only place
recording which ids exist — replace it with the retired-range note so the next id added
does not collide:

```cpp
//Phase 11 stdlib namespace ids: the math (70-94), io (110-114) and fs
//(120-127) blocks retired with the built-in standard library. The freed
//ids stay unallocated — intrinsic ids are only ever appended, never
//reused. String methods (95-106) survive as receiver-dispatched built-ins.
```

- [ ] **Step 5: un-pin the size guard and the count assertion**

`tools/source_size_guard/allowlist.json`: delete the objects whose `"path"` is
`src/vm/IntrinsicsFs.cpp`, `src/vm/IntrinsicsIo.cpp`,
`src/vm/IntrinsicsMath.cpp` (lines 62-76).

`tests/test_vm/test_stdlib.cpp:279-280` — the walk's basis was the table's row
count. Replace with the declaration count it actually protects:

```cpp
    //25 is stdlib/math.n's native declaration count: a deliberate tripwire,
    //so a declaration cannot silently disappear from the library source.
    CHECK(checked == 25,
        "every math native declaration must be exercised by this walk");
```

Also rename the test to what it now proves: `test_stdlib_table_full_dispatch` →
`test_stdlib_native_full_dispatch`. There are exactly two occurrences in
`tests/test_vm/test_stdlib.cpp` — the definition at `:224` and the call in the suite
runner at `:547` (this file uses a hand-written `main()` + `CHECK/PASS`, not a `TEST()`
macro), plus the module-tag prefix `"tbl_"` → `"native_"` at `:274`. Do not touch the
string-method walk's `"strtbl_"` prefix (`:415`) or its `kStringMethodIntrinsicCount`
assertion (`:420`) — that family survives.

- [ ] **Step 6: build, guard, full suite serially**

```bash
cmake -S . -B build-dev
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release
```
Expected: build exit 0; **63/63 passed** — the guard's last red clauses are exactly this
task's scope, so `no_builtin_stdlib` goes green here and `stdlib_generation` is already
retired (Task 3). If `stdlib_tests` fails on a math/io/fs call, the DLL path is not
covering something the VM used to — that is a real finding, stop and report it rather
than restoring a family.

- [ ] **Step 7: commit (ask the user first)**

```bash
git commit -m "refactor(vm): retire the math/io/fs intrinsic families

The 576 lines implementing math/io/fs inside the VM became unreachable when
library .n sources started compiling to qualified OP_CallFunc calls against
nlang_<ns>.dll: nothing emits INTR_Math_*, INTR_Io_* or INTR_FileSystem_*
any more. Keeping them would mean two implementations of io/math/fs that
can drift apart.

- delete IntrinsicsMath/Io/Fs.cpp, their dispatch arms and declarations
- free id blocks 70-94, 110-114, 120-127 stay unallocated (no renumbering)
- stdlib_tests walks stdlib/math.n declarations and now pins that count
  itself instead of borrowing it from the retired table"
```

## Task 5: Retire the signature-generation checker

**FOLDED INTO TASK 3 (Step 6), 2026-09-28.** The checker parses `kStdLibTable[] =` out of
`StdLib.h`, so it turns red in the same build that deletes the table; retiring it in a
separate later task would ship a red commit. Task 3 Step 6 carries its Steps 1-3 verbatim
(coverage check, ctest block + `git rm`, the `ExprResolverStdLib.cpp:2` header fix) and its
`git rm` lands in Task 3's commit. **Nothing in this task is left to execute** — keep it
numbered so Tasks 6-8's references stay readable.

**Files:**
- Delete: `tests/check_stdlib_generation.py` (136 lines)
- Modify: `tests/CMakeLists.txt:884-889` (the `stdlib_generation` block)

**Interfaces:**
- Consumes: Task 4 (nothing left for the script to compare against).
- Produces: nothing; the suite count drops by one test (65 → 64 relative to the
  start of this plan, i.e. the +1 guard and −1 generator cancel).

- [ ] **Step 1: read what the script pinned, and confirm it is superseded**

Run: `sed -n '1,40p' tests/check_stdlib_generation.py`
Expected: it parses `kStdLibTable[] =` out of `StdLib.h` (line 44) and cross-checks
`stdlib/*.n` `native` declarations against it (return types, param types, param
names). With the table deleted there is no second source to agree with: `stdlib/*.n`
is authoritative, and the behaviour of every declaration is proven by
`test_stdlib.cpp` (the real compile+run walks) and the `nvm` native e2e scripts.
Confirm that claim before deleting:
`ctest --test-dir build-dev/tests -C Release -R "stdlib|native|thirdparty" --output-on-failure`
Expected: all Passed.

- [ ] **Step 2: delete the ctest block and the script**

In `tests/CMakeLists.txt`, delete lines 884-889:

```cmake
    # stdlib/*.n declaration guard: files generated from kStdLibTable must
    # exist and carry matching signatures (return type + param types).
    add_test(NAME stdlib_generation
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_stdlib_generation.py)
    set_tests_properties(stdlib_generation PROPERTIES TIMEOUT 60)
```

```bash
git rm tests/check_stdlib_generation.py
```

- [ ] **Step 3: clear the last stale reference**

`src/compiler/builder/ExprResolverStdLib.cpp:2` still names `TryResolveStdLibCall`
in its file header, a function Phase 4a removed. Replace the header line with what
the file does now:

```
    ExprResolverStdLib.cpp — 库命名空间调用与跨模块限定名解析（TryResolveModuleQualified / 模块提示）。
```

Then confirm nothing else mentions either retired name:
`grep -rn "stdlib_generation\|check_stdlib_generation\|TryResolveStdLibCall" src include tests tools docs`
Expected: no hits.

- [ ] **Step 4: reconfigure and run the full suite serially**

```bash
cmake -S . -B build-dev
ctest --test-dir build-dev/tests -C Release
```
Expected: **63/63 passed** (63 = 62 before this plan, +`no_builtin_stdlib`,
−`stdlib_generation`).

- [ ] **Step 5: commit (ask the user first)**

```bash
git commit -m "test: retire the stdlib signature-generation checker

check_stdlib_generation.py pinned stdlib/*.n native declarations against
kStdLibTable. The table is gone, so the library sources are the only
signature authority and there is nothing to agree with; the real
compile-and-run walks in test_stdlib.cpp and the nvm native e2e cover the
declarations that matter."
```

## Task 6: Documentation

**Status: DONE — landed as `3bd962f` (2026-09-29).** en/zh intrinsics pages rewritten around
what actually still dispatches (`kStringMethodTable`, ids 95-106, plus the stream/container/
protocol arms — `math`/`io`/`fs` documented as library sources); the `kStdLibTable` transition
text and its `### Future directions` bullet are gone from both `language-spec/standard-library.md`
twins; `docs/{en,zh}/vm-architecture/library-mechanism.md` landed from the parked doc set with
§6 rewritten as "What the standard library is made of" (4c done) and §8 corrected so 4b-2's type
surface is recorded as **not in the tree**; roadmap gained the 统一库机制 phase entry and the
vm-architecture page count (14→15). Docs gate: **67 pytest passed** via the pinned env
`D:/dev/miniconda3/envs/py313/python.exe` (build-dev configures `NLANG_BUILD_DOCS=OFF`, so
`nlang_docs_pytest` is NOT one of the 63 ctest cases here — run the suite by hand as in Task 7
Step 3). `grep -rn "kStdLibTable" docs/` → no hits at all (the retirement record was rewritten
into present-tense prose rather than kept as a table row).

**Files:**
- Rewrite: `docs/user_manual/en/vm-architecture/standard-library-intrinsics.md` (62 lines) and
  `docs/user_manual/zh/vm-architecture/standard-library.md`… — **the zh twin is
  `docs/user_manual/zh/vm-architecture/standard-library-intrinsics.md` (56 lines)**
- Modify: `docs/user_manual/en/language-spec/standard-library.md:15-21` and `:200-204`
  (`### Future directions`), `docs/user_manual/zh/language-spec/standard-library.md:14` and
  `:184`
- Modify: `docs/user_manual/en/vm-architecture/library-mechanism.md:142-153` (§6 retirement
  table) and `docs/user_manual/zh/vm-architecture/library-mechanism.md:118-128`
- Modify: `docs/roadmap.md` (phase summary lines; `docs/` tree per the project's
  paired en/zh convention)

**Interfaces:**
- Consumes: the finished code state of Tasks 1-5.
- Produces: nothing functional. `nlang_docs_pytest` (public-text gate) and the
  mkdocs nav-parity check must stay green.

- [ ] **Step 1: rewrite the intrinsics page (both languages, one change)**

`standard-library-intrinsics.md` currently documents the signature table
(en `:13-15`, `:27-31`), the id-allocation table (en `:38-49`, rows `70-94`,
`110-114`, `120-127`) and the string methods. Replace the table passages so the
page describes what is left: `OP_CallIntrinsic` carries (a) the string-method
family, ids `95-106`, resolved through `kStringMethodTable`, and (b) the
collection/container and file-stream builtins. State plainly that `io`/`math`/`fs`
have no VM-internal implementation and link the library-mechanism page. Keep the
opcode-guard section (en `:51-62`) — it is still true. Mirror the result in the zh
file at the same section positions (zh `:11-12`, `:24-27`, `:33-44`).

- [ ] **Step 2: remove the transition language from the standard-library page**

en `:15-21` says library functions are "recognized at compile time against
`kStdLibTable`" and calls it "the last hardcoded piece"; en `:202-204` lists
"retire `kStdLibTable`" under `### Future directions`. Delete both: declarations are
read from `stdlib/*.n` through the symbol index, natives run from
`nlang_<ns>.dll`. zh `:14` and `:184` (淘汰 `kStdLibTable`) are the same text. Do not
write "previously/now" narrative — the docs describe the current engine only.

- [ ] **Step 3: fix the retirement table in the library-mechanism pages**

`docs/user_manual/en/vm-architecture/library-mechanism.md` §6 (lines 142-153) lists
`kStdLibTable`, `FindStdLibFunction`, "zero callers — deletion pending (phase 4c)"
and its §8 lists 4b-2 as landed. Update both: 4c is done as of this change, and the
in-progress 4b-2 qualified-type work must be described as **not yet in the tree**
until it is committed. Mirror in `docs/user_manual/zh/vm-architecture/library-mechanism.md`
(lines 118-128 and its remaining-work section).

- [ ] **Step 4: roadmap summary**

`docs/roadmap.md`: add one line under the phase list recording that the built-in
standard library is retired and `stdlib/*.n` + native DLLs are the only
implementation. Keep the en/zh pair and the existing tense of neighbouring entries.

- [ ] **Step 5: verify the docs gates**

```bash
ctest --test-dir build-dev/tests -C Release -R nlang_docs_pytest --output-on-failure
grep -c "vm-architecture/library-mechanism" mkdocs.en.yml mkdocs.zh.yml
grep -rn "kStdLibTable" docs/ | grep -v "vm-architecture/library-mechanism"
```
Expected: `nlang_docs_pytest ... Passed` (it runs
`tools/nlang-docs/tests/test_public_text.py`, the zero-legacy-text gate); each mkdocs
nav counts exactly `1` (both files already carry the library-mechanism entry from the
uncommitted docs change); the last grep reports **no output** — the retired table's
name survives only in the library-mechanism retirement record (one row in each
language), and no other page may still describe it as the call path.

- [ ] **Step 6: commit (ask the user first)**

```bash
git commit -m "docs: document that the standard library has no VM-internal implementation

Rewrite the intrinsics page around what still dispatches through
OP_CallIntrinsic (string methods, collection builtins, file streams), drop
the kStdLibTable transition text from the language spec, and mark phase 4c
done in the library-mechanism retirement table and roadmap."
```

## Task 7: Close-out verification

**Status: DONE (2026-09-29).** Guard green by hand; **serial ctest 63/63 twice** (101 s,
89 s) plus a third 63/63 after the round-1 comment fixes; docs pipeline **67 passed** on the
py313 env; residue greps clean — the retired names survive only inside
`tests/check_no_builtin_stdlib.py`'s own banned-pattern list and its clause-2 file list, which
is the guard naming what it forbids (the plan's "expected no hits" predicated on a scan that
excludes the guard itself). `StdLib.h` is 122 lines, not the estimated ~150.

- [ ] **Step 1: prove the mechanism, not just the tests**

Run: `D:/dev/miniconda3/python.exe tests/check_no_builtin_stdlib.py`
Expected: `no builtin stdlib: table, families and ids are retired; stdlib sources intact`

- [ ] **Step 2: full suite, serial, twice**

```bash
ctest --test-dir build-dev/tests -C Release
ctest --test-dir build-dev/tests -C Release
```
Expected: 63/63 both times (the second run proves no state leaks between runs).

- [ ] **Step 3: size guard and public text**

```bash
ctest --test-dir build-dev/tests -C Release -R "source_size_guard|nlang_docs_pytest" --output-on-failure
```
Expected: Passed. `StdLib.h` should now be roughly 150 lines (was 301).

- [ ] **Step 4: report the residue honestly**

List anything the plan left that the spec expected gone. Specifically check:
`grep -rn "kStdLibTable" docs src include tests tools` → expected no hits;
`grep -rn "INTR_Math_\|INTR_Io_\|INTR_FileSystem_" include` → expected no hits.

## Task 8: Phase audit loop — 4c closes only when a review round finds nothing new

**Status: DONE — closed after round 2 (2026-09-29).** Round 1 (`4c-reviewer-r1`,
`da5e4a8..3bd962f`): 0 Critical, 2 Important — both restore-step hazards created by 4c landing
on parked-WIP paths, recorded in the Global Constraints' restore protocol; 5 Minor, of which
three stale-comment findings (plus three more the coordinator found while working them) were
fixed as `1bce551`. Round 2 (`4c-reviewer-r2`, `da5e4a8..1bce551`): **0 Critical, 0 Important**
— the loop's stop condition — with an independently re-run 63/63 + 67 pytest + both hash
checks, and the extra surfaces (ndisasm/Disassembler, ModuleLoader/Import, nide/langservice
completion, packaging, the other guards, CHANGELOG/VERSION) verified clean. Round 2's
commit-by-commit narrative was wrong (it reconstructed rather than read the commits); its
re-runnable claims were checked by the coordinator and hold. Full log with dispositions:
`docs/dev/phase4c_audit_notes.md`.

**Update (2026-09-29, after the user said to continue and allowed 4b-2 and 4c to be finished
as one body of work):** the parked 4b-2 restore is done and committed as `ded5bbc`
(24 files, +694/−172 — the 21 WIP code files plus the three doc pages it needed rewritten).
The 3-way/`patch` route was abandoned for the 6 overlapping files because the `git show`
base blobs are LF while the worktree and the parked copies are CRLF, which turns every
per-file delta into one whole-file hunk that no merge tool can line up; the copies were
restored byte-for-byte and 4c's deletions re-applied inside them instead, then verified by
diffing each restored file against its parked copy (delta = 4c's edits, nothing else).
Baseline after the merge: **63/63 serial**, not the red 51/63 measured at `da5e4a8` — see
the restore-outcome block in Global Constraints. `temp/wip_4b2/` stays byte-intact
(21/21 + 4/4 OK) as the integrity record; the tree is clean at `ded5bbc`.

What remains open for this phase: the 4b-2 audit loop (`1bce551..ded5bbc`, round 1 running
as `4b2-reviewer-r1`). 4d is next after it closes.

Runs after Task 7. Required by the user's process rule (2026-09-28): every phase ends
with a review cycle that reports **no new issues**, and the next phase does not start
before that. Mechanism: `superpowers:requesting-code-review` — a fresh `general-purpose`
subagent per round, never this session reviewing its own work.

- [ ] **Step 1: pin the range and the WIP state**

```bash
git log --oneline -6
BASE_SHA=da5e4a8                        #4b, the commit 4c starts from
HEAD_SHA=$(git rev-parse HEAD)          #the last 4c commit (docs)
git diff --stat ${BASE_SHA}..${HEAD_SHA}
git status --porcelain                  #only mkdocs.{en,zh}.yml + the two untracked docs
cat temp/wip_4b2/files.txt
(cd temp/wip_4b2 && sha256sum -c SHA256SUMS)
```
Expected: the `--stat` list is exactly the files Tasks 1-6 named (plus nothing), and
`git status` shows **only** the doc changes — the 4b-2 work lives in `temp/wip_4b2/` and
is not in the tree. Record both outputs — the reviewer reads them, and a file that appears
in the diff without belonging to a task is a finding before the review even starts.

- [ ] **Step 2: dispatch the reviewer (round N)**

Fill `superpowers:requesting-code-review`'s `code-reviewer.md` template with:

- **DESCRIPTION**: "Phase 4c: the built-in standard library is retired — `kStdLibTable`
  and `StdLibEntry` deleted, the unreachable math/io/fs intrinsic families deleted with
  their ids, the reserved-namespace check relocated to the compiler's module registry,
  a static guard added (`tests/check_no_builtin_stdlib.py`), the generation-time guard
  removed, and the en/zh docs rewritten to describe only the current engine."
- **PLAN_OR_REQUIREMENTS**: `docs/dev/phase4c_plan.md` (Tasks 1-7, Global Constraints,
  Appendix A) plus the approved ruling in `docs/dev/phase4bcd_design.md` §2. Paste the
  plan's own acceptance lines, not a summary of your implementation.
- **BASE_SHA / HEAD_SHA**: from Step 1.
- **Extra checklist, in this order** (each is a way this phase could be wrong that the
  gates in Task 7 do not cover):
  1. *Deletion completeness vs. over-deletion*: `include/nlang/vm/StdLib.h` should now
     carry only the string-method receiver dispatch; anything that still *compiles*
     against a deleted family means a live path existed and was cut, not retired.
  2. *The user's parked 4b-2 work is byte-intact* — it is in `temp/wip_4b2/`, not the
     tree. `(cd temp/wip_4b2 && sha256sum -c SHA256SUMS)` must be all-OK, and no 4c task
     may have edited a file there or copied one back. A 4c change that silently overwrote
     a parked WIP file is Critical: it destroys work that exists nowhere else.
  3. *Tests still test the real thing*: the surviving stdlib coverage must be
     compile-and-run, not table introspection; no mock, no weakened assertion, no
     deleted case that was covering behaviour rather than the table.
  4. *Public-text gate*: no legacy-engine or "previously/now" narrative in the rewritten
     docs; the retired name may appear only as the retirement record.
  5. *Guard honesty*: `tests/check_no_builtin_stdlib.py`'s positive controls (25/5/8
     native decls) must fail if `stdlib/*.n` were emptied — check the script is not a
     tautology.
  6. *Commit hygiene*: Conventional Commits, English, no `temp/` or `build-dev/`
     content, `master` untouched, nothing pushed.

- [ ] **Step 3: work the findings, then re-verify**

Classify exactly as the skill does: **Critical/Important** are fixed now, each with its
own red/green step (write the failing check first if the finding is testable); **Minor**
goes into `docs/dev/phase4c_audit_notes.md` with one line saying why it is deferred. Every
fix ends with `cmake --build build-dev --config Release -j 8` and a **serial**
`ctest --test-dir build-dev/tests -C Release` → 63/63. Ask the user before committing
fixes (the standing rule), and commit them as their own `fix(4c):`-scoped change so the
next round can diff them.

- [ ] **Step 4: loop until a round is clean**

Dispatch a **fresh** reviewer (new subagent, no memory of the previous round) against
the extended range `${BASE_SHA}..$(git rev-parse HEAD)`. Repeat Steps 2-4 while any
Critical or Important finding comes back. Stop condition: one full round returns zero
Critical and zero Important findings. Record the round count and the final verdict
verbatim when reporting 4c done — "63/63" is not the phase's closing statement; a clean
round is.

- [ ] **Step 5: only then start 4d**

`docs/dev/phase4d_plan.md` Task 1 must not begin before this loop ends clean. If a finding
turns out to need a format or IDE change, say so and re-plan rather than smuggling it
into 4d.
