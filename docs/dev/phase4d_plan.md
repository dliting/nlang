# Phase 4d — Recorded library dependencies and change-aware rebuild

> **归属变更（2026-09-29）**：阶段划分重编，见 `docs/dev/phases_567_design.md`。本计划属于
> **阶段 7「依赖、过期与可见性」**，排在阶段 6（每源文件一份 `.nmod` ＋ `.npack` ＋链接）
> 之后，因此**尚未定稿、暂不执行**：本计划的核心是把「库源清单」写进整项目一份的合并产物，
> 而阶段 6 之后每个 `.nmod` 天生只对应一个源文件，清单退化成「一条来源路径＋一个校验和」，
> 过期检测随之变成逐文件；现在按合并产物实现，阶段 6 一落地就作废。
> 不依赖产物格式的两项（`runNccBuild` 异步化、`ndb` 读库源文件）可提前单独做。
> 下面 Step 里的 v1.13 号已被阶段 5 占用（阶段 5：类型身份限定化＋`entryPoint`，格式 12→13，
> 见 `docs/dev/phase5_design.md`）；阶段 6 会再抬一次 `.nmod` 并新增 `.npack` 格式。
> 本计划里的「`package`／命名」相关前提已随重划作废，不得据以开工。

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development
> or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** a `.nmod` records the library `.n` sources that took part in its
compilation, so nide rebuilds a standalone program when a library source is
edited — not only when the program's own source is newer.

**Architecture:** option A from the approved design: the compiler already knows
the exact set (`ModuleBuilder::m_inlinedLibraryFiles`, absolute paths of every
inlined library TU). Serialize it into the module as format v1.13, placed right
after the module name so a dependency probe is a bounded header read
(`ModuleLoader::PeekLibrarySources`) rather than a full parse, and let the IDE
compare those paths' mtimes against the module's own mtime (the make rule). The
loader floor rises to 13, so a pre-4d module is refused — which is exactly the
"rebuild it" signal the IDE needs.

**Tech Stack:** C++17, CMake (Visual Studio 17 2022, `Release`), Qt5 Widgets
(nide), hand-rolled `TEST/CHECK/PASS` harness plus QTest suites under
`tests/test_nide/`.

**Spec:** `docs/dev/phase4bcd_design.md` §3 (option A approved by the user 2026-09-28).

## Deviations from the design doc (deliberate, both approved-in-spirit)

1. The doc says "paths + mtimes". This plan serializes **paths only** and uses the
   `.nmod`'s own mtime as the build timestamp: `any(source.mtime > module.mtime)`
   is the make rule, and it removes a clock from the format (no stale recorded
   mtime to keep in sync, no two-file timestamp drift). Recorded mtimes would add
   a field whose only consumer re-reads the filesystem anyway.
2. The doc left how nide obtains the list open. It reads it through
   `ModuleLoader::PeekLibrarySources`, so `nide_mainwindow` gains a link on
   `nlang_vm` (static lib, no new deployed DLL). The alternative — an `ncc deps`
   query — is option B, already rejected.

## Global Constraints

- **Entry condition (user rule, 2026-09-28):** 4c's audit loop
  (`docs/dev/phase4c_plan.md` Task 8) has reported a round with zero Critical/Important
  findings. 4d closes the same way — Task 9 — and nothing after 4d starts until that
  verdict exists. A test count is never the phase's closing statement.
- No backward compatibility: the loader floor rises to v1.13 and pre-4d modules are
  refused outright with the existing "outdated; recompile" error.
- `.nmod` bumps raise floor **and** ceiling together — the discipline recorded at
  `src/vm/ModuleLoader.cpp:51-58` ("Every format bump must raise the ceiling
  alongside the floor").
- The recorded list is **sorted and deduplicated** before serialization: a module's
  bytes must not depend on `unordered_set` iteration order.
- `source_size_guard`: ≤500 lines/file, ≤50 lines/function
  (`src/vm/ModuleLoader.cpp` and `ModuleSaver.cpp` are already allowlisted; check
  `tools/source_size_guard/allowlist.json` rather than assuming).
- Commits: Conventional Commits, English. **Ask the user before every commit.**
  Never push, never touch `master`, never commit `temp/` or `build-dev/`.
- Verification is serial: `ctest --test-dir build-dev/tests -C Release`.
  `mainwindow_tests` + `nide_deploy_check` fail under `-j` (verified 2026-09-28).
- Python scripts run under the interpreter ctest is configured with:
  `PYTHON3_EXECUTABLE = D:/dev/miniconda3/python.exe` (`build-dev/CMakeCache.txt`).
  Plain `python` in this shell is the WindowsApps stub and fails with
  `Permission denied`.
- New user-visible strings: nide's `.ts` catalogs are hand-maintained
  (`src/tools/nide/CMakeLists.txt:196-203`, `qt5_add_translation` compiles them at
  build time), so any new `tr()` literal introduced by Tasks 5-6 must get an entry in
  both `src/tools/nide/translations/nide_en.ts` and `nide_zh.ts`.
- Task 6 changes when nide's F5 session exists: the `DebugClient` child is created
  after ncc exits, not inside the trigger. That is a documented consequence, and the
  affected test sites are enumerated in Task 6 Step 7 rather than discovered by a
  red run.
- IDE behaviour changes need an interactive check by the user, not only tests
  (CLAUDE.md). Each nide task ends with the exact manual steps to run.
- Phase 4b-2 (qualified *type* references, `ns.Type`) **landed as `ded5bbc` on
  2026-09-29**, so `src/vm/VmBackend.h`, `src/vm/backend/VmBackend.cpp` and the
  `src/compiler/builder/*` files Task 2 edits are ordinary committed tree files: no
  partial staging is needed (Appendix A of `docs/dev/phase4c_plan.md` stays relevant only if
  the user parks new work again). Suite baseline from `ded5bbc` onward is 63/63 serial.

---

## Task 1: `.nmod` v1.13 — the recorded library sources

**Files:**
- Modify: `include/nlang/vm/CompiledModule.h:43` (version), `:14-42` (append the
  v1.13 history entry), `:378` (new field after `enumNames`)
- Modify: `src/vm/ModuleSaver.cpp:47` (insert the section right after the module-name
  block, i.e. after `fs.write(mod.name.c_str(), nameLen);`)
- Modify: `src/vm/ModuleLoader.cpp:67` (floor), `:83` (insert the read right after
  the module-name block, before `// String constants`)
- Test: `tests/test_vm/test_vm.cpp:106-150` (`test_module_save_load`)
- Modify: `tests/test_vm/test_debugger.cpp:254-256,300-302,348-350` (fresh-stamp
  anchors), `tests/packaging/verify_package.py:51` (`NMOD_MINOR`)

**Interfaces:**
- Consumes: nothing.
- Produces: `CompiledModule::librarySources` (`std::vector<std::string>`, absolute
  paths, format `uint32 count`, then per entry `uint32 len + bytes`), and
  `NMOD_FORMAT_MINOR == 13`. Task 2 fills it, Task 3 reads it, Task 5 consumes it.

- [ ] **Step 1: write the failing round-trip test**

In `tests/test_vm/test_vm.cpp`, extend `test_module_save_load`. It builds a
`CompiledModule`, writes it at `:139` with `WriteCompiledModule(fs, mod)` into
`tmpPath`, re-reads at `:144` with `auto loaded = ModuleLoader::Load(tmpPath);`
and then compares field by field.

Add the sources just before the save block (`:135`, after
`mod.functions.push_back(std::move(func));`):

```cpp
    //v1.13: the library sources that took part in the build travel with the
    //module so a rebuild check can see them. Two entries: order must survive.
    mod.librarySources = {"E:/libs/one/a.n", "E:/libs/two/b.n"};
```

and the comparisons right after the existing `loaded.name` check (`:145`):

```cpp
    CHECK(loaded.librarySources == mod.librarySources,
        "librarySources round-trips");
    CHECK(loaded.librarySources.size() == 2, "two library sources recorded");
```

- [ ] **Step 2: run it and confirm it does not compile**

Run: `cmake --build build-dev --config Release --target test_vm`
Expected: `error C2039: "librarySources": 不是 "nlang::CompiledModule" 的成员` — the
field does not exist yet.

- [ ] **Step 3: add the field and bump the version**

`include/nlang/vm/CompiledModule.h` — after the `enumNames` member (`:378`) add:

```cpp
    //v1.13: absolute paths of every library .n inlined into this module
    //(<name>.n from the search path, stdlib/*.n included). Written right
    //after the module name so a reader can probe dependencies without
    //parsing the body; the compiler records them, the IDE compares mtimes.
    std::vector<std::string> librarySources;
```

Change `:43` to `inline constexpr uint16_t NMOD_FORMAT_MINOR = 13;` and append the
history entry above it, matching the style of the entries at `:36-42`:

```cpp
//v1.13 (library dependencies): CompiledModule::librarySources, a
//count-prefixed list of absolute paths of the library .n files inlined into
//this module, serialized right after the module name. Consumers compare those
//files' mtimes against the module's to decide whether a rebuild is due, so
//editing a library source invalidates its users. The loader refuses v1.12
//and older: they carry no list, and a module without it is by definition
//stale for that check.
```

- [ ] **Step 4: write the section (writer side)**

In `src/vm/ModuleSaver.cpp`, immediately after the module-name block (`:44-47`,
ending `fs.write(mod.name.c_str(), nameLen);`) insert — the shape mirrors the
`enumNames` writer at `:294-307`:

```cpp
    //v1.13 library sources (absolute paths). Placed here, not at the tail,
    //so ModuleLoader::PeekLibrarySources can read it without parsing the
    //body. Format: uint32 count, then per entry uint32 len + bytes.
    uint32_t libCount = static_cast<uint32_t>(mod.librarySources.size());
    fs.write(reinterpret_cast<const char*>(&libCount), sizeof(libCount));
    for (const auto& lib : mod.librarySources) {
        uint32_t len = static_cast<uint32_t>(lib.size());
        fs.write(reinterpret_cast<const char*>(&len), sizeof(len));
        fs.write(lib.c_str(), len);
    }
```

- [ ] **Step 5: read the section, and raise the floor**

In `src/vm/ModuleLoader.cpp` change the version gate at `:67` from
`minorVer < 12` to `minorVer < 13`, and rewrite the comment above it (`:60-66`) so
it describes v1.13 — v1.12 modules lack the `librarySources` section, so reading
one would misparse `stringConstants` as the dependency count.

Then insert, immediately after the module-name read (`:77-83`, ending
`fs.read(mod.name.data(), nameLen);`):

```cpp
    //v1.13 library sources. The floor is 13, so this section is always
    //present — no minorVer gate (a gate here would be dead code).
    uint32_t libCount = 0;
    fs.read(reinterpret_cast<char*>(&libCount), sizeof(libCount));
    if (!fs.good() || libCount > (1u << 20))
        throw std::runtime_error("Invalid module: bad library source count");
    mod.librarySources.resize(libCount);
    for (uint32_t i = 0; i < libCount; ++i) {
        uint32_t len = 0;
        fs.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (!fs.good() || len > (1u << 24))
            throw std::runtime_error("Invalid module: bad library source length");
        mod.librarySources[i].resize(len);
        fs.read(mod.librarySources[i].data(), len);
    }
```

- [ ] **Step 6: run the round-trip test**

Run: `cmake --build build-dev --config Release --target test_vm && ctest --test-dir build-dev/tests -C Release -R "^vm_tests$" --output-on-failure`
Expected: Passed.

- [ ] **Step 7: fix the format tests that pin the old stamp**

`tests/test_vm/test_debugger.cpp` has three anchors asserting the freshly written
module's minor version, and they must move to 13:

```bash
grep -n "0x0C\|minorVer 12" tests/test_vm/test_debugger.cpp
```
Expected before the edit: lines 254-256, 300-302, 348-350. In each of the three
blocks change `== 0x0C` to `== 0x0D` and the message to
`"fresh module should be stamped minorVer 13"`. Leave the patches at
`bytes[10] = 0x08 / 0x09 / 0x0A / 0x0B` (lines 212, 257, 303, 351) untouched —
those four tests prove old modules are refused, and 8/9/10/11 are all below the
new floor of 13.

`tests/packaging/verify_package.py:51`: `NMOD_MINOR = 12` → `13` (the packaged-nmod
check reads the stamp from the header).

- [ ] **Step 8: full suite, serial**

Run: `ctest --test-dir build-dev/tests -C Release`
Expected: 63/63 Passed. Any `debugger_tests` failure here means a stale module
fixture was committed somewhere — there is none (`.nmod` files live in `temp/` and
`build-dev/`), so a failure is a real format bug.

- [ ] **Step 9: commit (ask the user first)**

```bash
git add include/nlang/vm/CompiledModule.h src/vm/ModuleSaver.cpp src/vm/ModuleLoader.cpp tests/test_vm/test_vm.cpp tests/test_vm/test_debugger.cpp tests/packaging/verify_package.py
git commit -m "feat(vm): record participating library sources in .nmod v1.13

CompiledModule::librarySources is a count-prefixed list of the absolute
paths of every library .n inlined into a module, written right after the
module name so a consumer can probe dependencies without parsing the body.

A rebuild check needs to know which library files a module was built from:
comparing only the program source against the module misses an edit to a
library body, which changes every caller. The loader floor rises to 13 in
the same step - v1.12 carries no list, so it is outdated by definition."
```

## Task 2: The compiler fills the list

**Files:**
- Modify: `include/nlang/compiler/ModuleBuilder.h:172` area (add an accessor if the
  set stays private — prefer a const getter over a new public member)
- Modify: `src/compiler/ModuleBuilder.cpp:185-192` (the `VmBackend` injection block)
- Modify: `src/vm/VmBackend.h:86` (setter, **WIP file** → Appendix A). It writes
  `m_compiledModule.librarySources` (`:1003`), so no `VmBackend.cpp` change is needed
- Test: `tests/test_vm/test_thirdparty.cpp` (add a scenario assertion), and
  `tests/test_vm/test_library_source.cpp` is already a WIP file — do not add there

**Interfaces:**
- Consumes: `CompiledModule::librarySources` (Task 1),
  `ModuleBuilder::m_inlinedLibraryFiles` (existing, filled at
  `src/compiler/ModuleBuilderImports.cpp:116-137` by `ParseLibraryUnit`, which
  inserts `std::filesystem::absolute(...).lexically_normal().string()`).
- Produces: `VmBackend::SetLibrarySources(std::vector<std::string>)`; every emitted
  `.nmod` carries a sorted, deduplicated list of the library TUs it was built from.

- [ ] **Step 1: write the failing end-to-end test**

In `tests/test_vm/test_thirdparty.cpp`, add to the mixed-library scenario (after
`buildAndRun` succeeds in `TestMixedLibraryFromFixture`), reading the module the
same way the scenario already loads it:

```cpp
//The module must record the library sources that took part: this is what
//makes a later edit to mixlib.n visible to a rebuild check. stdlib io.n
//participates too (it is an inlined library unit), so assert containment
//and ordering rather than an exact set.
void TestLibrarySourcesRecorded() {
    const fs::path pkg = packageDir() / "deps";
    fs::create_directories(pkg);
    std::ofstream lib(pkg / "dep.n", std::ios::binary);
    lib << "namespace dep\n{\n  int twice(int x) { return x + x; }\n}\n";
    std::ofstream prog(pkg / "prog.n", std::ios::binary);
    prog << "import dep;\n"
            "int main() { return dep.twice(2) == 4 ? 0 : 1; }\n";

    CapturingIo ioCapture;
    CHECK(buildAndRun(pkg, ioCapture) == 0, "dependency scenario builds and runs");

    const std::string moduleName = pkg.filename().string();
    CompiledModule mod = ModuleLoader::Load(
        (pkg / (moduleName + ".nmod")).string());
    CHECK(!mod.librarySources.empty(), "module records library sources");
    //Suffix match, not equality: the compiler stores
    //absolute(path).lexically_normal() and only the compiler's own
    //normalisation decides the spelling (drive-letter case, separators).
    //What the rebuild check needs is a path that resolves to this file.
    const std::string wanted = (pkg / "dep.n").string();
    auto namesDep = [&](const std::string& s) {
        return s.size() >= wanted.size()
            && s.compare(s.size() - wanted.size(), wanted.size(), wanted) == 0;
    };
    CHECK(std::find_if(mod.librarySources.begin(), mod.librarySources.end(),
                       [&](const std::string& s) {
                           return s == wanted || namesDep(s);
                       }) != mod.librarySources.end(),
          "the imported library source is in the recorded list ("
          + std::to_string(mod.librarySources.size()) + " entries)");
    CHECK(std::is_sorted(mod.librarySources.begin(), mod.librarySources.end()),
          "recorded sources are sorted (a module's bytes must not depend on "
          "unordered_set iteration order)");
    //The recorded path must actually resolve, or Task 5's stat loop is
    //comparing against nothing.
    CHECK(fs::exists(wanted), "the scenario library file is where it was written");
}
```

Register it in `main()` next to `TestMixedLibraryFromFixture();` (`:203`) and add
`#include <algorithm>` to the file's includes.

- [ ] **Step 2: run it and confirm the red**

Run: `cmake --build build-dev --config Release --target test_thirdparty && ctest --test-dir build-dev/tests -C Release -R thirdparty_tests --output-on-failure`
Expected: FAIL on `module records library sources` (the field is written but never
filled, so the list is empty).

- [ ] **Step 3: hand the set to the backend**

In `include/nlang/compiler/ModuleBuilder.h`, next to the existing members, add the
accessor (the set stays private):

```cpp
	//Absolute paths of every library .n inlined into this build, sorted so the
	//serialized list is reproducible byte for byte.
	std::vector<std::string> SortedLibrarySources() const;
```

In `src/compiler/ModuleBuilderImports.cpp` (after `DiscoverLibraryUnits`), define it:

```cpp
//v1.13: the dependency list a module carries. Sorted because
//m_inlinedLibraryFiles is an unordered_set and the .nmod bytes must be
//reproducible; absolute because a rebuild check compares mtimes and a
//relative path resolves against whoever asks.
std::vector<std::string> ModuleBuilder::SortedLibrarySources() const
{
	std::vector<std::string> sorted(m_inlinedLibraryFiles.begin(),
		m_inlinedLibraryFiles.end());
	std::sort(sorted.begin(), sorted.end());
	return sorted;
}
```

Add `#include <algorithm>` to that file if it is not already there.

- [ ] **Step 4: add the backend setter (WIP file → Appendix A)**

In `src/vm/VmBackend.h`, after `SetLibraryIndex` (`:83-86`):

```cpp
    //v1.13: the library sources inlined into this build, recorded into the
    //emitted module so consumers can tell whether a library edit made it
    //stale. Called before SaveModule.
    void SetLibrarySources(std::vector<std::string> sources)
    {
        m_compiledModule.librarySources = std::move(sources);
    }
```

- [ ] **Step 5: call it from the builder**

In `src/compiler/ModuleBuilder.cpp`, inside the existing `dynamic_cast<VmBackend*>`
block (`:185-192`), after `SetLibraryIndex`:

```cpp
		//v1.13: record which library sources took part, so an edit to a
		//library body invalidates the modules built from it.
		vmBackend->SetLibrarySources(SortedLibrarySources());
```

- [ ] **Step 6: run the scenario test**

Run: `cmake --build build-dev --config Release -j 8 && ctest --test-dir build-dev/tests -C Release -R "thirdparty|library_source|ncc_thirdparty" --output-on-failure`
Expected: all Passed, including the new `TestLibrarySourcesRecorded`.

- [ ] **Step 7: full suite serially, then commit (ask the user first)**

Run: `ctest --test-dir build-dev/tests -C Release` → 63/63 Passed.

```bash
git commit -m "feat(compiler): record inlined library sources into the emitted module

ModuleBuilder already keeps the absolute path of every library .n it inlines
(ParseLibraryUnit dedupes on it). Sorted and handed to the backend at codegen
setup, it becomes the module's v1.13 dependency list.

The stdlib .n files are in that list too: io.n/math.n are ordinary inlined
library units now, so editing one rebuilds its users like any other package."
```

## Task 3: `ModuleLoader::PeekLibrarySources` — a bounded probe

**Files:**
- Modify: `src/vm/ModuleLoader.h:9` area (declaration)
- Modify: `src/vm/ModuleLoader.cpp` (factor the header read out of `Load`, add the
  probe)
- Test: `tests/test_vm/test_native_loader.cpp` is the loader-adjacent suite, but the
  format lives in `test_module_import`/`vm_tests`; add to
  `tests/test_vm/test_vm.cpp` next to `test_module_save_load`

**Interfaces:**
- Consumes: v1.13 layout (Task 1), filled list (Task 2).
- Produces: `static std::vector<std::string> ModuleLoader::PeekLibrarySources(
  const std::string& filePath)` — returns the recorded paths; throws
  `std::runtime_error` with the same wording `Load` uses for a bad/old/truncated
  module. Consumed by nide (Task 5) and ndisasm (Task 4).

- [ ] **Step 1: write the failing tests**

In `tests/test_vm/test_vm.cpp`, add a test and register it in the suite's run list:

```cpp
//Peek must return what Load sees, without parsing the body, and must reject
//what Load rejects: a pre-v1.13 module is stale, not silently usable.
void test_peek_library_sources()
{
    TEST(peek_library_sources);
    CompiledModule mod;
    mod.name = "peek";
    mod.librarySources = {"E:/libs/a.n", "E:/libs/b.n", "E:/libs/c.n"};

    const std::string path =
        std::filesystem::temp_directory_path().string() + "/nlang_peek.nmod";
    const std::string stalePath =
        std::filesystem::temp_directory_path().string() + "/nlang_peek_stale.nmod";
    {
        std::ofstream fs(path, std::ios::binary);
        CHECK(WriteCompiledModule(fs, mod), "write peek module");
        fs.close();
    }

    CHECK(ModuleLoader::PeekLibrarySources(path) == mod.librarySources,
        "peek returns the recorded sources in order");

    //Patch the serialized minor version (magic is 8 bytes, major occupies
    //bytes 8-9, minor bytes 10-11) down to v1.12 and re-read the file.
    {
        std::fstream rw(stalePath, std::ios::binary | std::ios::in
            | std::ios::out | std::ios::trunc);
        std::ifstream src(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(src)),
            std::istreambuf_iterator<char>());
        bytes[10] = static_cast<char>(0x0C);
        rw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        rw.close();
    }
    bool threw = false;
    try {
        ModuleLoader::PeekLibrarySources(stalePath);
    } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("outdated") != std::string::npos,
            "a stale module reports the outdated-version error, got: "
            + std::string(e.what()));
    }
    CHECK(threw, "peek refuses a v1.12 module");
    std::filesystem::remove(path);
    std::filesystem::remove(stalePath);
    PASS();
}
```

Register it the way the file's other tests are registered (its `main()`/run list
calls `test_module_save_load();` — add `test_peek_library_sources();` beside it).
`<filesystem>` and `<vector>` are already included by this test file; if not, add
them.

- [ ] **Step 2: run to confirm the red**

Run: `cmake --build build-dev --config Release --target test_vm && ctest --test-dir build-dev/tests -C Release -R "^vm_tests$" --output-on-failure`
Expected: compile error `PeekLibrarySources is not a member of ModuleLoader`.

- [ ] **Step 3: declare it**

`src/vm/ModuleLoader.h`, after `static CompiledModule Load(...)`:

```cpp
    //Read only the header + v1.13 library-source list of a .nmod: magic,
    //version, module name, dependency list. Deliberately not a full Load -
    //an IDE deciding whether a rebuild is due must not parse every function
    //body, and the section sits right after the module name for that reason.
    //Throws the same errors Load throws for an outdated, newer or truncated
    //module; the caller treats any throw as "rebuild".
    static std::vector<std::string> PeekLibrarySources(const std::string& filePath);
```

- [ ] **Step 4: factor the shared header read out of `Load`**

Move `Load`'s existing magic + version + name + v1.13-list block (`:45-83` up to
the end of the block added in Task 1 Step 5) into a file-local helper so the two
readers cannot drift:

```cpp
namespace {
//Open the file, verify magic and version bounds, read the module name and the
//v1.13 dependency list. Shared by Load and PeekLibrarySources: one place decides
//what a stale module means.
void ReadModuleHeader(std::ifstream& fs, uint16_t& majorVer, uint16_t& minorVer,
                      std::string& name, std::vector<std::string>& librarySources)
{
    // (body = the code moved out of Load, verbatim, including the
    //  "Truncated module file", "is outdated; recompile with current ncc"
    //  and "was written by a newer ncc" throws)
}
} // namespace
```

`Load` then calls `ReadModuleHeader(...)` first and continues with
`stringConstants`. Keep every error message string byte-identical; tests and users
match on them.

- [ ] **Step 5: implement the probe**

```cpp
std::vector<std::string> ModuleLoader::PeekLibrarySources(
    const std::string& filePath)
{
    std::ifstream fs(filePath, std::ios::binary);
    if (!fs.is_open())
        throw std::runtime_error("Cannot open module file: " + filePath);
    uint16_t majorVer = 0, minorVer = 0;
    std::string name;
    std::vector<std::string> librarySources;
    ReadModuleHeader(fs, majorVer, minorVer, name, librarySources);
    return librarySources;
}
```

- [ ] **Step 6: run the suite and check the size guard**

```bash
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release -R "vm_tests|source_size_guard" --output-on-failure
```
Expected: both Passed. If `source_size_guard` flags `ModuleLoader::Load` for length,
the move of the header block into `ReadModuleHeader` is what fixed it — that is the
intended effect, not a regression to work around.

- [ ] **Step 7: commit (ask the user first)**

```bash
git commit -m "feat(vm): add a bounded .nmod probe for the recorded library sources

ModuleLoader::PeekLibrarySources reads magic, version, module name and the
v1.13 dependency list, then stops - an IDE checking whether a build is
current must not parse every function body to read a list that sits at the
front of the file.

The header rules (floor, ceiling, truncation, name) moved into one
ReadModuleHeader helper shared with Load, so the two readers cannot drift
into disagreeing about what a stale module is."
```

## Task 4: Expose the list to humans and scripts (ndisasm)

**Files:**
- Modify: `src/tools/ndisasm/main.cpp:52-84` (usage, new mode, module header dump)
- Modify: `tests/CMakeLists.txt` (register the e2e near `version_ndisasm`, `:859-867`)
- Create: `tests/check_ndisasm_library_sources.py`

**Interfaces:**
- Consumes: `CompiledModule::librarySources` via `ModuleLoader::Load`
  (`main.cpp:78`), and `PeekLibrarySources` for the machine mode.
- Produces: `ndisasm <module.nmod>` printing a `library sources:` block, and
  `ndisasm --library-sources <module.nmod>` printing one path per line and nothing
  else (a scriptable contract: exit 0 and a clean list, or non-zero and stderr).

- [ ] **Step 1: add the human dump**

In `src/tools/ndisasm/main.cpp`, right after `std::cout << "module: " << module.name << "\n";`
(`:84`):

```cpp
    //v1.13: which library sources this module was built from.
    if (module.librarySources.empty()) {
        std::cout << "library sources: (none)\n";
    } else {
        std::cout << "library sources:\n";
        for (const auto& lib : module.librarySources)
            std::cout << "  " << lib << "\n";
    }
```

- [ ] **Step 2: add the machine mode**

In the argument parsing block (`:66-74`), before `modulePath` handling:

```cpp
    //Machine-readable dependency list: one path per line, for tooling that
    //only needs to know what a module was built from.
    bool librarySourcesOnly = false;
    if (std::string(argv[1]) == "--library-sources" && argc >= 3) {
        librarySourcesOnly = true;
        modulePath = argv[2];
    }
```

and, after the module loads, branch before the full dump:

```cpp
    if (librarySourcesOnly) {
        for (const auto& lib : module.librarySources)
            std::cout << lib << "\n";
        return 0;
    }
```

Extend the usage text at `:54-56` with
`       ndisasm --library-sources <module.nmod>\n`.

- [ ] **Step 3: write the e2e check**

```python
#!/usr/bin/env python3
"""ndisasm reports the library sources a module was built from (v1.13).

Builds a program that imports a library package with ncc, then asks ndisasm
- as a user would - which sources took part. The machine mode is the
contract under test, the human dump is checked for the same path.
"""
import pathlib
import subprocess
import sys
import tempfile

NDISASM = pathlib.Path(sys.argv[1]).resolve()
NCC = pathlib.Path(sys.argv[2]).resolve()
STDLIB = pathlib.Path(sys.argv[3]).resolve()

work = pathlib.Path(tempfile.mkdtemp(prefix="nlang_ndisasm_"))
(work / "dep.n").write_text(
    "namespace dep\n{\n  int twice(int x) { return x + x; }\n}\n",
    encoding="utf-8")
(work / "prog.n").write_text(
    "import dep;\nint main() { return dep.twice(2); }\n", encoding="utf-8")

built = subprocess.run(
    [str(NCC), "build", str(work / "prog.n"), "-o", str(work / "prog.nmod"),
     "-I", str(work), "-s", str(STDLIB)],
    capture_output=True, text=True)
if built.returncode != 0:
    print(built.stdout + built.stderr)
    sys.exit("ncc failed to build the fixture")

machine = subprocess.run(
    [str(NDISASM), "--library-sources", str(work / "prog.nmod")],
    capture_output=True, text=True)
if machine.returncode != 0:
    print(machine.stderr)
    sys.exit("ndisasm --library-sources failed")

lines = [l for l in machine.stdout.splitlines() if l.strip()]
recorded = [pathlib.Path(l).resolve() for l in lines]
if (work / "dep.n").resolve() not in recorded:
    sys.exit(f"dep.n missing from {lines}")
if recorded != sorted(recorded):
    sys.exit(f"recorded sources are not sorted: {lines}")

human = subprocess.run(
    [str(NDISASM), str(work / "prog.nmod")],
    capture_output=True, text=True)
if "library sources:" not in human.stdout:
    sys.exit("ndisasm's module dump does not print the dependency list")
print(f"ok: {len(lines)} library sources reported, dep.n among them")
```

- [ ] **Step 4: register it**

Insert after the `version_ndisasm` block (`tests/CMakeLists.txt:859-867`), reusing
the same target-file and stdlib variables that block already uses:

```cmake
    # v1.13: ndisasm reports the library sources a module was built from.
    add_test(NAME ndisasm_library_sources
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_ndisasm_library_sources.py
            $<TARGET_FILE:ndisasm> $<TARGET_FILE:ncc> ${PROJECT_SOURCE_DIR}/stdlib)
    set_tests_properties(ndisasm_library_sources PROPERTIES TIMEOUT 120)
```

- [ ] **Step 5: reconfigure and run**

```bash
cmake -S . -B build-dev
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release -R "ndisasm" --output-on-failure
```
Expected: `ndisasm_library_sources ... Passed`, `version_ndisasm ... Passed`.

- [ ] **Step 6: full suite serially, then commit (ask the user first)**

Run: `ctest --test-dir build-dev/tests -C Release` → 64/64 (the new e2e).

```bash
git commit -m "feat(tools): ndisasm prints a module's recorded library sources

The human dump lists them under the module header; --library-sources prints
one path per line for tooling. Until now the dependency list written into
v1.13 modules had no way to be inspected except by writing a test against
ModuleLoader."
```

## Task 5: nide rebuilds when a library source changed

**Files:**
- Modify: `src/tools/nide/MainWindowBuildRun.cpp:181-196` (the standalone staleness
  check) and add the helper
- Modify: `src/tools/nide/MainWindow.h:331` area (declaration)
- Modify: `src/tools/nide/CMakeLists.txt:177-188` (`nide_mainwindow` links
  `nlang_vm`)
- Test: `tests/test_nide/test_searchpath_integration.cpp` (it already owns the
  standalone-plus-global-library fixture: `writeGreetLib` `:75`, `openSnippet`
  `:89`, `persistGlobalPaths` `:109`)

**Interfaces:**
- Consumes: `ModuleLoader::PeekLibrarySources` (Task 3).
- Produces: `bool MainWindow::standaloneBuildCurrent(const QString& source,
  const QString& output) const` — one place that decides whether a standalone
  program needs a rebuild, used by `runStandaloneFile`.

- [ ] **Step 1: write the failing test**

In `tests/test_nide/test_searchpath_integration.cpp`: add a library helper next to
`writeGreetLib` (`:75`), then a test slot after `globalLibraryDirIndexedAndBuilt`
(`:161`). This suite's idiom is the real tool chain — `act(window, ...)` triggers,
`inExec` + `acceptFileDialog` answer the modal, `QTRY_VERIFY_WITH_TIMEOUT` waits on
the output pane (30000 ms is the value the neighbouring real-build tests use).

```cpp
//A library whose body the test can edit: twice(2) is 4 with bonus 0,
//9 with bonus 5. The namespace doubles as the import name.
QString writeLibDep(const QString& baseDir, int bonus) {
    const QString libDir = QDir(baseDir).filePath("libs");
    QDir().mkpath(libDir);
    const QByteArray source = QByteArray("namespace libdep {\n"
        "    int twice(int x) { return x + x + ")
        + QByteArray::number(bonus) + "; }\n}\n";
    writeFile(QDir(libDir).filePath("libdep.n"), source.constData());
    return libDir;
}
```

```cpp
//Run must rebuild a standalone program when an imported library source
//changed: the v1.13 module records the library paths, the IDE compares
//them against the module's timestamp.
void standaloneRunFollowsLibraryEdit() {
    QTemporaryDir work;
    const QString libDir = writeLibDep(work.path(), 0);
    persistGlobalPaths({libDir});          //before the window: reindex

    MainWindow window;
    const QString srcPath = QDir(work.path()).filePath("solo_librun.n");
    CodeEditor* src = openSnippet(window, srcPath,
        "import libdep;\n"
        "public int main() {\n"
        "    return libdep.twice(2);\n"
        "}\n");
    QVERIFY(src != nullptr);

    //Standalone builds land in the per-stem temp slot (same path the
    //sibling real-build tests use); start from nothing.
    const QString nmod =
        QDir(QDir::temp()).filePath("nlang-nide/solo_librun.nmod");
    QFile::remove(nmod);
    act(window, "actStartRunning")->trigger();
    QTextEdit* out = window.findChild<QTextEdit*>("txtExecuteOut");
    QVERIFY(out != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(
        out->toPlainText().contains("exited with code 4"), 30000);

    //Backdate the module and the program source before the library edit.
    //The rule is an mtime compare and NTFS can resolve a build and the
    //writes around it into the same second, so the timestamps are set
    //rather than waited for: module = now-60s, source = now-120s (so the
    //program source itself stays 'older than the module' and cannot be
    //what trips the rebuild), library = its natural write (now-60s+):
    //only the recorded library source is out of date.
    const QDateTime builtAt = QFileInfo(nmod).lastModified();
    QVERIFY(QFile::setModTime(nmod, builtAt.addSecs(-60)));
    QVERIFY(QFile::setModTime(srcPath, builtAt.addSecs(-120)));
    //The fixture's own assumptions, stated so a failure names them:
    //the program source must look older than the module (it cannot be
    //what trips the rebuild), the library source newer.
    QVERIFY2(QFileInfo(srcPath).lastModified() <
                 QFileInfo(nmod).lastModified(),
             "backdating left the program source newer than the module");
    QVERIFY2(QFileInfo(QDir(libDir).filePath("libdep.n"))
                 .lastModified() > QFileInfo(nmod).lastModified(),
             "the library source is not newer than the backdated module");

    writeLibDep(work.path(), 5);           //twice(2) is 9 now
    act(window, "actStartRunning")->trigger();
    //Before this task the module counted as current (the program source
    //was untouched), so nvm re-ran the stale build and printed code 4.
    QTRY_VERIFY_WITH_TIMEOUT(
        out->toPlainText().contains("exited with code 9"), 30000);

    //The rebuild made the module current again: a third Run must not
    //build, so its timestamp stays where the second run left it.
    const QDateTime rebuiltAt = QFileInfo(nmod).lastModified();
    act(window, "actStartRunning")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        out->toPlainText().contains("exited with code 9"), 30000);
    QCOMPARE(QFileInfo(nmod).lastModified(), rebuiltAt);

    QFile::remove(nmod);
    persistGlobalPaths({});                // restore
}
```

No registration step: this suite runs through `QTEST_MAIN`, so a new
`private slots:` function is discovered automatically.

Add `#include <QDateTime>` and `#include <QTextEdit>` to that file's include list
(`QFile`, `QFileInfo`, `QDir`, `QTemporaryDir`, `QTest` are already there).

- [ ] **Step 2: run it and confirm the behavioural red**

The test compiles immediately (it exercises only public actions), and fails on
behaviour:

Run: `cmake --build build-dev --config Release --target test_searchpath_integration && ctest --test-dir build-dev/tests -C Release -R searchpath_integration --output-on-failure`
Expected: FAIL at the second `QTRY_VERIFY_WITH_TIMEOUT` — the pane contains
`exited with code 4`, not `9`, because `runStandaloneFile` compared the program
source alone and re-ran the stale module. If it fails *earlier* (the first Run),
the library did not resolve at all — check `persistGlobalPaths` ran before the
`MainWindow` was constructed, since the index is built at startup.

- [ ] **Step 3: link the format reader in**

`src/tools/nide/CMakeLists.txt`, in the `nide_mainwindow` link list (`:177-188`),
add after `nlang_version`:

```cmake
        nlang_vm      #ModuleLoader::PeekLibrarySources: is this build current?
```

`nlang_vm` exposes only `include/` as a PUBLIC include dir and keeps its own
`src/vm` directory PRIVATE — which is where `ModuleLoader.h` lives (`src/vm/`), so
the consumer has to add it the way every `.nmod` reader does
(`src/tools/ncc/CMakeLists.txt:7-11`). After the link list (`:188`):

```cmake
#ModuleLoader.h sits in src/vm, a PRIVATE include dir of nlang_vm; the tools
#that read a .nmod each add the directory (ncc does the same).
target_include_directories(nide_mainwindow
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
)
```

Both are static libraries (no `BUILD_SHARED_LIBS` anywhere in this build), so no
new DLL enters the deployment set — `nide_deploy_check` proves that (Step 6). If
the `nide` executable link fails on a VM symbol, do **not** duplicate the reader:
`nlang_vm`'s own private deps (`nlang_compiler`, `nlang_langservice`) propagate as
link-only interface libraries for static targets, and ncc's link list
(`src/tools/ncc/CMakeLists.txt:13-20`) is the set that is known to close.

- [ ] **Step 4: implement the decision**

In `src/tools/nide/MainWindow.h`, after `standaloneNmodPath` (`:331`):

```cpp
    //True when `output` exists and is newer than the program source and every
    //library source recorded in it (v1.13). A module that cannot be read -
    //pre-v1.13, truncated, written by a newer ncc - counts as not current:
    //rebuilding is the correct answer in all of those cases.
    bool standaloneBuildCurrent(const QString& source,
                                const QString& output) const;
```

In `src/tools/nide/MainWindowBuildRun.cpp`, implement and replace the inline check
at `:191-196`:

```cpp
bool MainWindow::standaloneBuildCurrent(const QString& source,
                                        const QString& output) const {
    const QFileInfo outInfo(output);
    if (!outInfo.exists())
        return false;
    if (outInfo.lastModified() < QFileInfo(source).lastModified())
        return false;
    std::vector<std::string> sources;
    try {
        sources = ModuleLoader::PeekLibrarySources(output.toStdString());
    } catch (const std::exception&) {
        return false;
    }
    for (const std::string& lib : sources) {
        const QFileInfo libInfo(QString::fromStdString(lib));
        //A library source that vanished also counts as changed: the rebuild
        //then fails with ncc's own "cannot find library" diagnostic, which is
        //more useful than silently running a module built against it.
        if (!libInfo.exists()
            || libInfo.lastModified() > outInfo.lastModified())
            return false;
    }
    return true;
}
```

```cpp
    //D2: unlike the project Run (which asks for a manual build first),
    //a missing or outdated module is rebuilt here automatically - outdated
    //including any recorded library source edited since the build.
    if (!standaloneBuildCurrent(filePath, output)) {
        if (!buildStandaloneFile(filePath))
            return;
    }
```

Add `#include "ModuleLoader.h"` and `#include <vector>` to the .cpp if the IDE's
include list does not already carry them.

- [ ] **Step 5: run the test**

Run: `cmake --build build-dev --config Release -j 8 && ctest --test-dir build-dev/tests -C Release -R "searchpath_integration|mainwindow" --output-on-failure`
Expected: Passed. The fixture sets the timestamps instead of sleeping, so a failure
here means the compare itself is wrong, not the clock — re-read
`standaloneBuildCurrent` before touching the fixture. The three existing
`standalone*` tests in `test_mainwindow.cpp` (`:1387`, `:1402`, `:1497`) are the
neighbours most likely to notice a behaviour change; they must stay green.

- [ ] **Step 6: full suite serially, including the deploy check**

Run: `ctest --test-dir build-dev/tests -C Release`
Expected: 64/64 Passed — `nide_deploy_check` included, which is the proof that
linking `nlang_vm` added no deployment requirement.

- [ ] **Step 7: ask the user for the interactive check (CLAUDE.md requires it)**

Give exactly these steps: open nide → File > Open a standalone `.n` that imports a
library from a directory on the search path → Run (it builds and runs) → edit the
library `.n`, save, and Run again with no manual build → expected: the program
reflects the edited library body (proving it rebuilt). Also: Run twice in a row
without edits → expected: the second Run does not print a build log line.

- [ ] **Step 8: commit (ask the user first)**

```bash
git commit -m "feat(nide): rebuild a standalone program when a library source changed

The outdated check compared the program source against the .nmod only, so
editing an imported library .n kept running the stale build. v1.13 modules
record the sources that took part, and the IDE now compares those too -
reading only the module header through ModuleLoader::PeekLibrarySources, so
the check stays a file-stat loop rather than a parse.

A module that cannot be read at all (v1.12 or older, truncated, newer ncc)
counts as not current, so the first Run after this change rebuilds it."
```

## Task 6: Non-blocking builds in nide

**Files:**
- Modify: `src/tools/nide/MainWindowBuildRun.cpp:62-80` (`runNccBuild` →
  `startNccBuild`), `:82-103` (`buildProject`), `:159-179`
  (`buildStandaloneFile`), `:181-213` (`runStandaloneFile`, splits out `runModule`)
- Modify: `src/tools/nide/MainWindowDebug.cpp:40-54` (`prepareDebugTarget` stops
  building), `:77-95` (`startDebugSession` splits into kick-off + `launchDebugSession`)
- Modify: `src/tools/nide/MainWindow.h:313-331` (declarations) and `:423` area (the
  `m_building` flag), `updateMenuState()` in `MainWindow.cpp:318-330`
- Modify: `src/tools/nide/MainWindowBuildRun.cpp:20-40` (`on_actBuild_triggered`,
  `on_actStartRunning_triggered` — the two menu entry points)
- Test: `tests/test_nide/test_mainwindow.cpp` (one new slot, plus the eight
  `actBuild` and four `actStartDebug` sites that assert immediately after the
  trigger — enumerated in Step 5)
- Test: `tests/test_nide/test_searchpath_integration.cpp:156`
  (`globalLibraryDirIndexedAndBuilt` asserts its status line right after
  `actBuild`)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `void MainWindow::startNccBuild(const QStringList& args, const QString& workDir,
    std::function<void(bool, QString)> onDone)` — the only ncc launcher, gated on
    `m_building`; `bool building() const`.
  - `void MainWindow::buildProject(ProjectNode&, std::function<void(bool)> onDone)`
    and `void MainWindow::buildStandaloneFile(const QString&,
    std::function<void(bool)> onDone)` — both return immediately; `onDone(true)`
    means the module is on disk.
  - `void MainWindow::runModule(const QString& output)` (was the tail of
    `runStandaloneFile`) and `bool MainWindow::launchDebugSession(const QString&)`
    (was the tail of `startDebugSession`).

- [ ] **Step 1: write the failing test**

In `tests/test_nide/test_mainwindow.cpp`, add a slot in the
`//--- build & run (real ncc + nvm) ---` block (`:1955`), right after
`testBuildAndRun` (`:1957`). `buildProject`/`startNccBuild`/`building` are all
private, so the test drives the public action and reads the observable consequence
of the gate: the QAction enabled state. Under the synchronous implementation
`trigger()` returns only after ncc exited and the actions came back on — that is
the red.

```cpp
//A build must not own the UI thread: actBuild has to return while ncc is
//still running, with the build/run actions gated off until the process
//finishes.
void testBuildLeavesTheEventLoopRunning() {
    MainWindow window;
    QTemporaryDir dir;

    inExec([&] { acceptProjectDialog("App", dir.path()); });
    act(window, "actNewProject")->trigger();
    inExec([&] { acceptNewFileDialog("main.n"); });
    act(window, "actAddNewFile")->trigger();
    currentCode(window)->setPlainText(kMainSource);

    act(window, "actBuild")->trigger();
    //Nothing between the trigger and these checks spins the event loop, so
    //an asynchronous build is provably still in flight right here.
    QVERIFY2(!act(window, "actBuild")->isEnabled(),
             "actBuild must stay disabled while ncc runs");
    QVERIFY2(!act(window, "actStartRunning")->isEnabled(),
             "Run must be gated while a build is in flight");

    //The completion path re-enables both and reports the same status line
    //the synchronous build used.
    QVERIFY(QTest::qWaitFor([&] {
        return window.statusBar()->currentMessage()
                   == QString("Build succeeded")
            && act(window, "actBuild")->isEnabled();
    }, 15000));
    QVERIFY(act(window, "actStartRunning")->isEnabled());
    QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath("App.nmod")));
    QFile::remove(QDir(dir.path()).filePath("App.nmod"));
    QFile::remove(QDir(dir.path()).filePath("App.nproj"));
}
```

No registration: this suite is driven by the hand-written `main()` at the bottom of
the file (`QTest::qExec`), which discovers `private slots:` automatically.

- [ ] **Step 2: confirm the red**

Run: `cmake --build build-dev --config Release --target test_mainwindow && ctest --test-dir build-dev/tests -C Release -R mainwindow --output-on-failure`
Expected: the new slot fails on the **first** `QVERIFY2` — `actBuild` is enabled
again by the time `trigger()` returns, because `runNccBuild` blocked until ncc
exited. The other slots stay green at this point (Step 1's test is the only one
that looks mid-build).

- [ ] **Step 3: declare the asynchronous build**

In `src/tools/nide/MainWindow.h`, replace the synchronous block at `:313-318`
(the `//Synchronous ncc build...` comment, `buildProject`,
`saveProjectForBuild`, `prepareBuildOutput`, `runNccBuild`) — keep
`saveProjectForBuild` and `prepareBuildOutput` exactly as they are, they are
preludes, not the build — with:

```cpp
    //Run ncc without blocking the event loop: ncc writes diagnostics on
    //stderr, so the channels are merged before reading. onDone runs on the
    //UI thread with (succeeded, whole log). Re-entrancy is gated: one build
    //at a time, reported by building().
    void startNccBuild(const QStringList& args, const QString& workDir,
                       std::function<void(bool, QString)> onDone);
    bool building() const { return m_building; }
```

and change the two builders (`:314`, `:325`) from `bool` returns to
continuations — "did it succeed" no longer exists at the point of return:

```cpp
    //Save, then build asynchronously: onDone(true) means the module is on
    //disk and the compile pane + status line already report it.
    void buildProject(ProjectNode& project, std::function<void(bool)> onDone);
    ...
    //ncc build <file> -o <nmod> into the temp slot; onDone(true) when the
    //module is on disk.
    void buildStandaloneFile(const QString& filePath,
                             std::function<void(bool)> onDone);
```

plus, next to `runStandaloneFile` (`:328`):

```cpp
    //Start the program under nvm. Split out of runStandaloneFile so a
    //chained build can call it when its module appears.
    void runModule(const QString& output);
```

and in the debug block (`:340-341`) replace

```cpp
    bool startDebugSession();
    QString prepareDebugTarget();
```

with

```cpp
    //Resolve the F5 target, rebuild it asynchronously and launch ndb from
    //the completion; a session that cannot start leaves the window idle.
    void startDebugSession();
    //The session proper: create the client and launch ndb on `modulePath`.
    //The tail of the old startDebugSession (:82-97), unchanged.
    bool launchDebugSession(const QString& modulePath);
```

`std::function` needs `#include <functional>` in `MainWindow.h` (it already
carries `<memory>` for the DebugClient unique_ptr; add the other next to it).
Finally add the member next to `QProcess m_executed;` (`:423`):

```cpp
    bool m_building = false;   //startNccBuild in flight
```

Steps 3-6 are one change seen from four angles: after this step the tree does **not**
compile (the declarations no longer match the definitions, and the callers still read
`bool` returns). Do not run a build checkpoint until Step 8; a mid-task build failure
here is the plan working, not a mistake.

- [ ] **Step 4: implement it**

In `src/tools/nide/MainWindowBuildRun.cpp`, replace `runNccBuild` (`:62-80`) with:

```cpp
void MainWindow::startNccBuild(const QStringList& args,
                               const QString& workDir,
                               std::function<void(bool, QString)> onDone) {
    if (m_building)
        return;                        //one build at a time; the menu already
    m_building = true;                 //reflects it through updateMenuState
    updateMenuState();

    auto* ncc = new QProcess(this);
    auto log = std::make_shared<QString>();
    ncc->setProcessChannelMode(QProcess::MergedChannels);
    ncc->setWorkingDirectory(workDir);
    QObject::connect(ncc, &QProcess::readyReadStandardOutput, ncc,
        [ncc, log] { *log += QString::fromLocal8Bit(ncc->readAll()); });
    QObject::connect(ncc, &QProcess::errorOccurred, ncc,
        [this, ncc, log, onDone](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;                //finished() still reports a real exit
            *log += tr("Failed to start '%1'.").arg(toolPath("ncc"));
            ncc->deleteLater();
            m_building = false;
            updateMenuState();
            onDone(false, *log);
        });
    QObject::connect(ncc,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), ncc,
        [this, ncc, log, onDone](int exitCode, QProcess::ExitStatus status) {
            //Anything not drained by readyReadStandardOutput is still there.
            *log += QString::fromLocal8Bit(ncc->readAll());
            ncc->deleteLater();
            m_building = false;
            updateMenuState();
            onDone(status == QProcess::NormalExit && exitCode == 0, *log);
        });
    ncc->start(toolPath("ncc"), args);
}
```

Add `#include <functional>` and `#include <memory>` to the file.

- [ ] **Step 5: convert the two builders and their callers**

`buildProject` (`:82-103`) and `buildStandaloneFile` (`:159-179`) keep their save +
`prepareBuildOutput` prelude; every early return becomes `onDone(false); return;`
and the synchronous tail (`:96-102` / `:172-178`) becomes:

```cpp
    startNccBuild(args, project.projectDir(),
        [this, onDone](bool succeeded, const QString& log) {
            m_ui->txtCompileOut->append(log);
            m_ui->statusBar->showMessage(
                succeeded ? tr("Build succeeded") : tr("Build failed"));
            onDone(succeeded);
        });
```

(workDir is `QFileInfo(filePath).absolutePath()` for the standalone one, and its
`onBuilt` continuation is what the Run path needs — pass the success flag, not a
silent `onBuilt()`, so a failed rebuild does not launch nvm on a stale module.)

The four call sites change like this:

- `on_actBuild_triggered` (`:28-37`) — the result is only reported through the
  pane and the status line, so both get a no-op continuation:
  `buildProject(*project, [](bool) {});` /
  `buildStandaloneFile(standalone, [](bool) {});`
- `runStandaloneFile` (`:188-196`) — its body from `:197` on moves verbatim into
  the new `runModule(const QString& output)` (that is the `txtExecuteOut->clear()`,
  `showOutputPage`, `setWorkingDirectory`, `m_executed.start(...)` and
  `updateMenuState()` sequence, unchanged; leave `runProject`'s near-identical tail
  at `:115-128` alone — its working directory is the project dir, not the module
  dir, and merging the two would be an unrequested refactor). The remaining
  decision becomes:

```cpp
    //D2: unlike the project Run (which asks for a manual build first), a
    //missing or outdated module is rebuilt here automatically - outdated
    //including any recorded library source edited since the build. The
    //launch chains onto the build: the module does not exist yet for a
    //first-time source.
    if (!standaloneBuildCurrent(filePath, output)) {
        buildStandaloneFile(filePath, [this, output](bool succeeded) {
            if (succeeded)
                runModule(output);
        });
        return;
    }
    runModule(output);
```

- `MainWindowDebug.cpp:40-54` (`prepareDebugTarget`) — delete the function; its
  resolve half moves into `startDebugSession`, its build half becomes the async
  call. `startDebugSession` (`:77-98`) turns into:

```cpp
void MainWindow::startDebugSession() {
    //Resolve first: the module path is derivable before ncc runs, so the
    //F5 target does not have to be built to know where it will sit. A
    //project target wins over a standalone one (the same rule the old
    //prepareDebugTarget applied).
    if (ProjectNode* project = currentProject()) {
        m_debugBaseDir = project->projectDir();
        const QString modulePath = outputFilePath(*project);
        buildProject(*project, [this, modulePath](bool succeeded) {
            if (succeeded)
                launchDebugSession(modulePath);
        });
        return;
    }
    const QString standalone = currentStandaloneTarget();
    if (standalone.isEmpty())
        return;                    //the action was disabled without a target
    m_debugBaseDir = QFileInfo(standalone).absolutePath();
    const QString modulePath = standaloneNmodPath(standalone);
    buildStandaloneFile(standalone, [this, modulePath](bool succeeded) {
        if (succeeded)
            launchDebugSession(modulePath);
    });
}

bool MainWindow::launchDebugSession(const QString& modulePath) {
    //The old startDebugSession body, from createDebugClient() on: the
    //`modulePath.isEmpty() || !QFileInfo::exists(modulePath)` guard is
    //gone because onDone(true) is what proves the module exists.
    createDebugClient();
    ...unchanged through `return true;`
}
```

  `on_actStartDebug_triggered` (`:37`) keeps calling `startDebugSession()` — it
  already ignores the return value.

- [ ] **Step 6: make the menu state honest**

In `MainWindow::updateMenuState()` (`MainWindow.cpp:288`, the `canBuild` block at
`:317-330` and the Start/Run/Debug reads that follow), treat `m_building` as busy:
while a build runs, `actBuild`, `actStartRunning`, `actStartDebug` and the step
actions stay off. Two concrete reasons, both in the neighbouring comment's style:
a second Run during a build starts a competing ncc on the same output file and
Windows refuses writes to a loaded image; and a mid-build F5 would queue a second
launch continuation against the module the first one is still writing. `updateMenuState()`
is called by `startNccBuild` at kick-off and in both completion handlers (Step 4),
so no new call site is needed.

- [ ] **Step 7: convert the tests that assumed a blocking build**

Twelve sites assert something the moment `trigger()` returns. Each gets the
suite's existing wait idiom inserted after the trigger — for builds, wait on the
status line; for F5, wait on the `DebugClient` child, which is what `updateMenuState`
and the later assertions read:

```cpp
        act(window, "actBuild")->trigger();
        QVERIFY(QTest::qWaitFor([&] {
            return window.statusBar()->currentMessage()
                       == QString("Build succeeded");
        }, 15000));
```

```cpp
        startDebug->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.findChildren<DebugClient*>().isEmpty(), 30000);
```

Exact sites in `tests/test_nide/test_mainwindow.cpp` (line numbers before this
task's edits; a `// synchronous QProcess` / `// synchronous build` comment sits on
or beside each and must be reworded to say the completion is what is waited for):

| line | test | what follows the trigger today |
|---|---|---|
| `:1394` | `testBuildStandaloneFileWritesTempNmod` | `QVERIFY(QFileInfo::exists(nmod))` |
| `:1415` | `testBuildStandaloneHonorsBuildOutputDir` | `QVERIFY(QFileInfo::exists(nmod))` |
| `:1435` | `testBuildProjectHonorsBuildOutputDir` | exists + `QCOMPARE` status |
| `:1489` | `testBuildStandaloneDiagnosticsReachOutput` | pane contains "Error" — wait for `QString("Build failed")` instead |
| `:1538` | `testTreeSelectedStandaloneRowWinsOverActiveEditor` | exists(nmodA) **and** `!exists(nmodB)` — wait for nmodA first, then the negative check is meaningful |
| `:1969` | `testBuildAndRun` | `QCOMPARE` status (keep the assertion, add the wait) |
| `:2019` | `testBuildFailingSource` | `QCOMPARE QString("Build failed")` |
| `:2047` | `testStopRunningKillsProcess` | exists(App.nmod) |
| `:2228` | `testDebugActionStates` | Start/Stop/step enabled states |
| `:2336` | `testStopDebugDuringSession…` | `actStopDebug->trigger()` — stopping a session that has not been created yet is the bug this wait prevents |
| `:2356` | (window-close teardown) | `QVERIFY(!findChildren<DebugClient*>().isEmpty())` |
| `:2413` | (Running-state session test) | same child check |

`:2266`, `:2315`, `:2384`, `:2431` already reach a `QTRY_*`/`QTEST_` wait before
they read session state; leave them alone. `tests/test_nide/
test_searchpath_integration.cpp:156` (`globalLibraryDirIndexedAndBuilt`) needs the
build wait too — and Task 5's new slot is already asynchronous (it waits on the
execute pane), so it needs no change.

- [ ] **Step 8: run the IDE suites**

```bash
cmake --build build-dev --config Release -j 8
ctest --test-dir build-dev/tests -C Release -R "mainwindow|searchpath|nide" --output-on-failure
```
Expected: Passed. A slot that now times out is a site Step 7 missed, not a broken
build: `grep -n "actBuild\|actStartDebug" tests/test_nide/*.cpp` and look for an
assertion on the line after the trigger. Do not make any of this synchronous again
to quiet a test — the only accepted fix is a wait.

- [ ] **Step 9: full suite serially**

Run: `ctest --test-dir build-dev/tests -C Release` → 64/64.

- [ ] **Step 10: interactive check with the user**

Open nide with a large-ish standalone source that imports a library. Steps: Run →
while ncc is compiling, try scrolling the editor and typing → expected: the UI
stays responsive (before this change it froze for the whole build). Then click Run
twice quickly → expected: no overlapping build, no "cannot write the output file"
error. Then edit a library `.n`, Run → expected: rebuilt and the new behaviour runs.
Then F5 on the same file → expected: the debugger stops at a breakpoint in the
library source after the build finishes (the chained launch, not a stale session).

- [ ] **Step 11: commit (ask the user first)**

```bash
git commit -m "fix(nide): run ncc asynchronously

runNccBuild waited on the process with waitForFinished(-1), freezing the UI
for the whole compile - tolerable for a quick build, not for a library-heavy
one. The build now reports through a completion handler, one build at a time
(building()), and both chained launches wait for it: Run starts nvm when the
module appears, F5 creates its DebugClient the same way, so a first-time
source still builds before it executes. updateMenuState treats a build as
busy, which keeps a second Run or F5 from racing ncc on the output file."
```

## Task 7: Debugger coverage for library sources

**Files:**
- Test: `tests/test_vm/test_debugger.cpp` (new case in the existing suite)
- Read: `src/tools/ndb/SourceCache.cpp:29-43` (resolution order: as-recorded path,
  then `m_moduleDir / filename`), `src/vm/DebugSessionController.cpp:98-137`
  (`AddBreakpoint(file, line, exactFile)` with `NormalizePath` suffix matching),
  `src/tools/ndb/DebugSession.cpp:396` (`"No source file for this frame.\n"`)

**Interfaces:**
- Consumes: v1.13 modules whose library bodies carry absolute `sourceFile` paths
  (per-function path already serialized since v1.9, `ModuleSaver.cpp:148-152`).
- Produces: a regression test proving a breakpoint and source listing inside a
  library `.n` work through ndb's machine protocol. No production change is
  expected; if this test fails, the fix belongs in the two files named above.

- [ ] **Step 1: write the coverage test**

The suite already has the harness (`scratchDir()` at `:60`, `buildConsumer()` at
`:115` which puts the scratch dir on the import path, `loadBuilt()` at `:140`, and
`RunSession()` at `:1058` which drives `DebugSession` + `DebugSessionController`
in-process). Add a consumer-shaped twin of `RunSession` and the case, after
`test_session_breakpoint_hit` (`:1120-1145`):

```cpp
//RunSession's shape for a consumer TU that imports a library .n from the
//scratch dir (buildConsumer already puts that dir on the import path, so
//DiscoverLibraryUnits finds <name>.n and inlines it).
static std::string RunConsumerSession(const std::string& tag,
    const std::string& source, const std::string& commands)
{
    BuildOutcome b = buildConsumer(tag, source);
    if (!b.ok) return "BUILD FAILED: " + b.diagnostics;
    CompiledModule mod = loadBuilt(tag);
    std::ostringstream out;
    std::istringstream in(commands);
    DebugSession session(mod,
        (scratchDir() / (tag + ".nmod")).string(), in, out);
    DebugSessionController controller(mod, session);
    session.SetController(&controller);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.Execute(mod);
    return out.str();
}

//A library body is ordinary compiled code carrying its own absolute
//sourceFile (v1.9): a breakpoint addresses the library file and the listing
//reads it back off disk. This is the ndb half of phase 4d - no engine change,
//a pinned behaviour the library mechanism depends on.
void test_session_library_source_breakpoint()
{
    TEST(session_library_source_breakpoint);
    {   //namespace file: the return statement is line 5
        std::ofstream lib(scratchDir() / "sess_dep.n", std::ios::binary);
        lib << "namespace sess_dep\n"
               "{\n"
               "    int twice(int x)\n"
               "    {\n"
               "        return x + x;\n"
               "    }\n"
               "}\n";
    }
    std::string out = RunConsumerSession("sess_lib_main",
        "import sess_dep;\n"                    //1
        "int main() {\n"                        //2
        "    int a = sess_dep.twice(21);\n"     //3
        "    return a;\n"                       //4
        "}\n",
        "b sess_dep.n:5\nc\nc\nl\nc\nc\n");
    CHECK(out.find("BUILD FAILED") == std::string::npos, out);
    //Set-time report ("Breakpoint 1 at <func> (<file>:<line>)"), matched on
    //the library file and line so the qualified function spelling is free to
    //be whatever the module's function table says.
    CHECK(out.find("Breakpoint 1 at ") != std::string::npos
        && out.find("sess_dep.n:5") != std::string::npos,
        "a breakpoint inside a library source resolves, got: " + out);
    CHECK(out.find("return x + x;") != std::string::npos,
        "the listing reads the library source back, got: " + out);
    PASS();
}
```

Register `test_session_library_source_breakpoint();` in the run list next to
`test_session_breakpoint_hit();` (`:2222-2225` area). Count the stops from the first
real run and add the trailing `c`s so the script always reaches normal exit: the
file's own warning at `:1054-1057` is that EOF while frozen means `q` and would kill
the whole test binary (the suite would then report a crash, not a failure).

- [ ] **Step 2: run it**

Run: `cmake --build build-dev --config Release --target test_debugger && ctest --test-dir build-dev/tests -C Release -R debugger_tests --output-on-failure`
Expected: Passed. Three named failure modes, in the order to check them:
`BUILD FAILED: ... import ... not found` means the library source did not resolve —
`buildConsumer`'s header comment (`:111-114`) predates Phase 4a and still talks
about a compiled `.nmod`; read `src/compiler/builder/ModuleBuilderImports.cpp`'s
resolution order (`m_ImportDirs` → library `.n`) rather than adding a second build
helper. A missing `Breakpoint 1 at` means the suffix match in `SourceFileMatches`
(`DebugSessionController.cpp:21-30`) does not accept the library's absolute
`sourceFile`. A listing with no `return x + x;` is `SourceCache.cpp:29-43` — the
as-recorded path is what the module carries, so this is where a library path goes
quietly wrong.

- [ ] **Step 3: pin the degraded listing when the recorded source is gone**

A committed case, not a manual rename — this is the state a user reaches when a
library file was deleted after the build (Task 5 rebuilds in the IDE, ndb on an old
module must stay understandable). It needs the module **already built**, because a
second `RunConsumerSession` would rebuild it and the build would now fail with ncc's
own "cannot find library" error — a different scenario. `RunSession` (`:1058-1074`)
splits cleanly at `:1063`, so add the build-free sibling next to it:

```cpp
//Session over an already-built <tag>.nmod: for the cases that must change
//the sources on disk after the build (a recorded library source that has
//gone) without rebuilding them.
static std::string RunBuiltSession(const std::string& tag,
    const std::string& commands)
{
    CompiledModule mod = loadBuilt(tag);
    std::ostringstream out;
    std::istringstream in(commands);
    DebugSession session(mod,
        (scratchDir() / (tag + ".nmod")).string(), in, out);
    DebugSessionController controller(mod, session);
    session.SetController(&controller);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.Execute(mod);
    return out.str();
}
```

Then the case (the script is Step 1's: one `c` releases the initial stop, the next
runs into the library frame, `l` lists it, the rest exits):

```cpp
//A module keeps the library path, but the file may be gone by the time
//anyone debugs it. The listing must degrade to line numbers without text,
//not hang, crash or silently print nothing at all.
void test_session_library_source_missing()
{
    TEST(session_library_source_missing);
    {
        std::ofstream lib(scratchDir() / "sess_gone.n", std::ios::binary);
        lib << "namespace sess_gone\n{\n    int twice(int x)\n    "
               "{\n        return x + x;\n    }\n}\n";
    }
    std::string out = RunConsumerSession("sess_gone_main",
        "import sess_gone;\nint main() { return sess_gone.twice(2); }\n",
        "c\n");                     //build + one clean continue to exit
    CHECK(out.find("BUILD FAILED") == std::string::npos, out);

    std::filesystem::remove(scratchDir() / "sess_gone.n");
    out = RunBuiltSession("sess_gone_main", "b sess_gone.n:5\nc\nc\nl\nc\nc\n");
    CHECK(out.find("return x + x;") == std::string::npos,
        "no source text is printed for a missing file, got: " + out);
    CHECK(out.find("Unknown command") == std::string::npos,
        "the session still parses its commands, got: " + out);
    PASS();
}
```

The third `CHECK` is deliberately loose: assert only that nothing broke, then **read
what the listing actually says** before tightening it — Step 3's whole point is to
find out whether the degradation is readable (see the note under the Run line).

Run: `ctest --test-dir build-dev/tests -C Release -R debugger_tests --output-on-failure`
Expected: Passed. If the second half prints line numbers with no explanation at all,
that is a real finding worth a production change: `SourceCache::Lines` returning an
empty vector is indistinguishable from an empty file, and the message the user needs
("source not found: <path>") belongs in `DebugSession::DoList` — add it there, keep
the "numbers" fallback, and note the deviation in the commit message.

- [ ] **Step 4: full suite serially, then commit (ask the user first)**

```bash
git commit -m "test(debugger): a library source is breakpoint-able and listable

Library bodies carry their absolute sourceFile, so ndb's file:LINE
breakpoints and listings reach them with no engine change; this pins that
behaviour and the degraded response when a recorded source has gone."
```

## Task 8: Documentation, changelog

**Files:**
- Modify: `docs/user_manual/en/vm-architecture/library-mechanism.md` + `docs/user_manual/zh/...` (the
  remaining-work section: 4d landed; describe the recorded list and the rebuild
  rule) — these pages are still uncommitted from the earlier docs change; this task
  finalises and commits them together with the nav entries already in
  `mkdocs.en.yml` / `mkdocs.zh.yml`
- Modify: `docs/user_manual/en/language-spec/standard-library.md` + zh (one sentence: editing a
  `stdlib/*.n` makes programs that use it rebuild)
- Modify: `docs/user_manual/en/vm-architecture/bytecode-format.md` (or whichever page documents
  the `.nmod` layout — `grep -rn "v1.12\|NMOD" docs/user_manual/en` to find it) with the v1.13
  section and the new floor
- Modify: `CHANGELOG.md` + `CHANGELOG.zh-CN.md`
- `VERSION`: leave at `0.7.4` — verify first (Step 5)

**Interfaces:**
- Consumes: the finished state of Tasks 1-7.
- Produces: nothing functional. The en/zh nav-parity gate and `nlang_docs_pytest`
  (public-text) must stay green.

- [ ] **Step 1: document the recorded dependency list and the rebuild rule**

In both library-mechanism pages, add a short section: the compiler records every
inlined library source (absolute path, sorted) into the module; a rebuild is due
when the program source, or any recorded library source, is newer than the module;
a module that predates v1.13 is treated as not current and is rebuilt on the next
Run; stdlib `.n` files are in that list, so editing `stdlib/math.n` invalidates
programs that use it.

Then rewrite §8 "Remaining work (design status as of 2026-09-29)" (en page, and the same
section in the zh page) into what the mechanism now is: 4d is the last open item there, so
once it lands §8 becomes a short "what the IDE does with the recorded list" paragraph plus
the retained non-goals. Two accuracy points this section must not lose on the way: 4b-2
(qualified *type* references across libraries) landed as `ded5bbc` and §8/§3 already
describe it as landed — do not demote it back to open; and the `.nmod`-mtime rule is
paths-only by decision, so do not write that timestamps are serialized. Keep en and zh in
the same change with identical section structure.

- [ ] **Step 2: document the format change**

The `.nmod` layout lives in `docs/user_manual/en/vm-architecture/module-serialization.md` and
`docs/user_manual/zh/vm-architecture/module-serialization.md` (found with
`grep -rln "1\.12\|NMOD_FORMAT" docs/user_manual/en docs/user_manual/zh`). In each of the two:

- the layout block (`en:12-21`): `uint16 minorVer = 12` → `= 13`, and add
  `string[] librarySources` on the line after `string moduleName` — that is where
  the writer puts it (`src/vm/ModuleSaver.cpp:44-46`, the module-name block)
- the **Version history** paragraph (`en:23`): prepend a v1.13 entry in the
  existing style — a layout change, the section sits right after the module name,
  it holds the absolute paths of every inlined library source (stdlib `.n` files
  included), sorted and de-duplicated, and "a v1.12 module from an older ncc records
  no dependency list, so the loader refuses minor < 13 outright — older modules must
  be recompiled", which is the sentence that makes the IDE's rebuild rule work.

Keep the zh page structurally identical (same block, same history position).

- [ ] **Step 3: changelog, both languages**

`CHANGELOG.md` and `CHANGELOG.zh-CN.md` currently head with
`## [0.7.4] - Unreleased`. Add under its `### Added` / `### 新增`:
- en: `- .nmod records the library sources it was built from (format v1.13), and nide rebuilds a standalone program when one of them changes`
- en: `- ndisasm prints a module's recorded library sources; ndisasm --library-sources for scripts`
- en: `- nide builds no longer block the UI`
- zh: matching three lines (中文 phrasing, same order).
Add under `### Fixed` / `### 修复`:
- en: `- a standalone run in nide no longer executes a stale build after an imported library source was edited`

- [ ] **Step 4: translations for any new UI string**

The catalogs are Qt `.ts` files (`<source>` + `<translation>` per message — **not**
gettext, so there are no `msgid`s), and they are hand-maintained:
`src/tools/nide/translations/nide_en.ts` / `nide_zh.ts` carry 193 messages each
today, and `lrelease` compiles them into the binary at build time
(`src/tools/nide/CMakeLists.txt:196-203`), so no manual `lrelease` run is needed.

```bash
grep -c "<source>" src/tools/nide/translations/nide_en.ts src/tools/nide/translations/nide_zh.ts
grep -on "tr(\"[^\"]*\")" src/tools/nide/MainWindowBuildRun.cpp src/tools/nide/MainWindowDebug.cpp | sort -u
```
Expected: both catalogs keep the same `<source>` count as each other, and every
literal the second command prints exists in both files. Every string this plan
reuses (`Build succeeded`, `Build failed`, `Failed to start '%1'.`,
`'%1' does not exist. Build the project first.`) is already catalogued — Tasks 5-6
introduce no new user-visible text, so the expected outcome is that this step
changes nothing. If a step did add a literal, add the `<message>` block to both
catalogs (`lupdate` regenerates skeletons; keep the existing context/name shape) —
never leave one language untranslated.

- [ ] **Step 5: version decision**

Run: `cat VERSION && head -12 CHANGELOG.md`
Expected: `0.7.4` and an `Unreleased` `## [0.7.4]` heading. Since the release
heading is still unreleased and already carries the phase's 0.7-series work, do not
invent a new number — leave `VERSION` alone. If the heading is instead already
dated/released, stop and ask the user which version this phase lands in; do not
bump unilaterally. (`verify_package.py` checks the tool `--version` output against
`VERSION`, and Task 1 already moved `NMOD_MINOR` there.)

- [ ] **Step 6: verify the docs gates, then the full suite**

```bash
ctest --test-dir build-dev/tests -C Release -R "nlang_docs|docs" --output-on-failure
grep -c "vm-architecture/library-mechanism" mkdocs.en.yml mkdocs.zh.yml
grep -rn "kStdLibTable" docs/ | grep -v vm-architecture/library-mechanism
ctest --test-dir build-dev/tests -C Release
```
Expected: docs tests Passed; each mkdocs nav lists the library-mechanism page
exactly once; the third grep reports no output (the retired table's name lives only
in the two library-mechanism retirement rows — 4c set that, this task must not
re-introduce it elsewhere); full suite 64/64.

- [ ] **Step 7: commit (ask the user first)**

```bash
git commit -m "docs: describe the recorded library sources and the rebuild rule

Add the library-mechanism section on the v1.13 dependency list and the
make-rule staleness check, document the format change where the .nmod layout
lives, note that editing stdlib/*.n invalidates its users, and record the
change-aware rebuild, the async nide build and the ndisasm output in both
changelogs."
```

## Task 9: Phase audit loop — 4d closes only when a review round finds nothing new

Required by the user's process rule (2026-09-28): a phase ends with a review cycle that
reports **no new issues**, and nothing after it starts until that happens. Same
mechanism as `docs/dev/phase4c_plan.md` Task 8: `superpowers:requesting-code-review`, a
fresh `general-purpose` subagent per round, this session never reviewing its own work.

- [ ] **Step 1: pin the range and the WIP state**

```bash
BASE_SHA=<the commit 4d starts from, i.e. 4c's last commit>
HEAD_SHA=$(git rev-parse HEAD)
git log --oneline ${BASE_SHA}..${HEAD_SHA}
git diff --stat ${BASE_SHA}..${HEAD_SHA}
git status --porcelain              #clean tree expected; 4b-2 landed as ded5bbc
```
Expected: the diff touches exactly the files Tasks 1-8 name, and `git status --porcelain`
is empty apart from `temp/`. 4b-2 is committed, so no file this plan edits is WIP any
more; if the user parks new work before 4d closes, re-apply Appendix A instead.
An unexpected hunk in `src/vm/VmBackend.h` or `src/compiler/ModuleBuilder.cpp` is
Critical before the reviewer even arrives.

- [ ] **Step 2: dispatch the reviewer (round N)**

Fill the `code-reviewer.md` template with DESCRIPTION = "Phase 4d: `.nmod` v1.13 records
the inlined library sources, `ModuleLoader::PeekLibrarySources` probes them from the
header, ndisasm prints them, nide rebuilds a standalone program when one of them is
newer than the module, nide's builds run without blocking the UI (Build, Run and F5 all
chain onto the completion), and ndb's library-source debugging is pinned by tests.";
PLAN_OR_REQUIREMENTS = `docs/dev/phase4d_plan.md` + `docs/dev/phase4bcd_design.md` §3 (option A),
quoting the plan's acceptance lines rather than summarising the implementation.

**Extra checklist** — the ways this phase specifically goes wrong, in order:
  1. *Format discipline*: floor and ceiling moved together; the new section is read at
     exactly the offset the writer puts it (a mismatch survives a round-trip test and
     bites only a real file); the list is sorted **and** deduplicated, so the bytes do
     not depend on `unordered_set` order.
  2. *The probe cannot drift from the loader*: `ReadModuleHeader` must be the single
     place the version rules live (Task 3); a duplicated compare inside
     `PeekLibrarySources` is an Important finding.
  3. *Staleness rule honesty*: `standaloneBuildCurrent` treats an unreadable module as
     not current and uses `>` (not `>=`) for library sources; `runProject`'s
     ask-to-build-first behaviour is unchanged — this phase was never about the project
     path.
  4. *Async build re-entrancy*: one build at a time, `m_building` cleared on **both**
     completion paths, `updateMenuState` called at kick-off and on completion, and no
     caller left assuming a `bool` build result. A continuation that can silently drop a
     Run or an F5 launch is Critical.
  5. *Test conversions are complete*: `grep -n "actBuild\|actStartDebug" tests/test_nide/*.cpp`
     and check that every trigger followed by an assertion has a wait. A test that passes
     by racing is an Important finding even while it is green.
  6. *The tree holds nothing uncommitted*: 4b-2 landed in `ded5bbc`, so any uncommitted
     file at review time is either an oversight of this plan or the user's new WIP — name
     it in the report either way (Appendix A covers the parking procedure).
  7. *Docs*: en/zh pages changed in the same commit, both mkdocs navs, the `.nmod` layout
     block matching the writer byte for byte, and the "paths only — the module's own
     mtime is the build timestamp" rule not oversold as recorded mtimes.
  8. *No new UI string without both `.ts` catalogs*; commit hygiene as in 4c.

- [ ] **Step 3: work the findings, then re-verify**

Critical/Important fixed now with their own red/green step, Minor into
`docs/dev/phase4d_audit_notes.md` with a one-line reason. Every fix ends with
`cmake --build build-dev --config Release -j 8` plus a **serial**
`ctest --test-dir build-dev/tests -C Release` → 64/64. Ask the user before committing;
fixes land as their own commit so the next round can diff them.

- [ ] **Step 4: loop until a round is clean**

Fresh reviewer each round against the extended range; repeat while any Critical or
Important finding returns. Stop condition: one full round with zero of both. Report the
round count and the final verdict verbatim — the phase is not "done" on the strength of
a test count.

- [ ] **Step 5: phase close-out, once the loop is clean**

```bash
ctest --test-dir build-dev/tests -C Release      #×2, serial: 64/64 both
ctest --test-dir build-dev/tests -C Release -R "no_builtin_stdlib|source_size_guard|nlang_docs" --output-on-failure
git status --porcelain                            #only temp/ and build-dev/ residue
```
Plus, with the user: build a program importing a package, edit the package's `.n` body,
and confirm (a) `ndisasm --library-sources` lists it, (b) nide's next Run rebuilds it,
(c) the program's behaviour changed, (d) the UI stayed responsive during the build.
Then `docs/roadmap.md` gains the phase line (en/zh pair) and neither master nor any
remote is touched.
