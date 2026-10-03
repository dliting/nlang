# Library Mechanism: Search Paths, Source Inlining and Native Modules

This page records the design of NLang's library system as it stands after
the evolution rounds (phases 1–6): how a library is found, parsed, compiled
and executed, and why the mechanism takes this shape. It is a design note
for maintainers; user-facing usage lives in
[Standard Library](../language-spec/standard-library.md).

## 1. Guiding principle

**The standard library and third-party libraries are one mechanism.** A
standard library is a `stdlib/*.n` file on the search path like any other
library; the compiler and the VM do not distinguish them. This mirrors
Python, where the standard library is ordinary source on `sys.path`, and
Java/C#, where managed source and native (JNI/P-Invoke) implementations
coexist inside one package. Nothing in the compiler or the VM hardcodes a
standard-library signature: the declaration files and the native DLLs are
the only implementation.

## 2. Library format

A library is one directory on the search path containing:

```
mylib/
  mylib.n            # the library: declarations, NLang bodies, or both
  nlang_mylib.dll    # optional: native implementation of `native` declarations
```

The library's surface is the `<pkg>.n` file itself; the package name is
the file's path relative to the matched search root
(`vendor/graphics.n` under root `R` is the package `vendor.graphics`),
and the file carries no wrapper syntax. Functions come in two kinds,
freely mixed (a *mixed library*):

- **plain NLang functions** — have bodies; compiled into their own
  package's unit image (a member of e.g. `stdlib.npkg`) and linked with
  the consumer at run time, then executed as bytecode;
- **`native` functions** — signature plus documentation comment only (no
  body); at run time dispatched through the host ABI into
  `nlang_<package>.dll` (section 5; the DLL is named by the package
  segment before the first dot, so a `native` in a multi-segment
  package is a compile-time diagnostic).

The standard library follows the same shape: `stdlib/io.n`, `math.n`,
`fs.n` are hand-written authoritative declaration files (the io/math/fs
packages are pure native today), built as `nlang_math.dll`,
`nlang_io.dll`, `nlang_fs.dll` from `src/native/`.

## 3. Compilation model: full source inlining

Library `.n` files reached through an `import` — single-segment or
dotted — are parsed **completely** — bodies included — as *library
translation units* and merged into the same AST root as the project's
units (`src/compiler/ModuleBuilderImports.cpp`,
`src/compiler/builder/ModuleRegistry*`). Build order:

1. parse project sources (collecting imports);
2. **library discovery to a fixpoint**: a worklist seeded with the
   project's imports; a dotted name `a.b.c` locates `<root>/a/b/c.n`
   under the first search root that has it (more than one root offering
   the same package is a duplicate-package error naming both paths), and
   each located file is (a) signature-indexed
   (`langservice::SymbolIndex`, under the matched-root package) and (b)
   fully parsed as a library TU, whose own imports join the worklist —
   so a third-party library may depend on other libraries. A match that
   is already a project source is not re-inlined;
3. register all TUs; library TUs carry an `isLibrary` flag and derive
   their package from the matched root;
4. expand aliases, build import gates, load external modules
   (`.ncu`/`.npkg`);
5. resolve and generate code **per unit** — library units do not
   participate in codegen: the consumer's image only carries import
   slots, and the library's code is provided by its own package at run
   time.

Consequences of one shared model:

- **Package-qualified calls compile as ordinary calls.** A call
  `mylib.f(...)` emits `OP_CallFunc` with the fully-qualified name
  `pkg.f`; run time dispatches on the callee's own nature — a bytecode
  body, or `isNative` → the DLL. There is no signature-table call path
  and no per-library codegen branch anymore.
- **Top-level function names in the function table are fully
  qualified** for packaged functions (including library natives), so
  the runtime lookup `m_natives["pkg.f"]` matches the DLL's exported
  registrations.
- **Types defined by a library are usable**: library members merge into
  the root with their owner tags, so a library's class/struct/enum/
  interface resolve at the consumer side like project types (inheritance,
  virtual dispatch and enum methods included). A type position writes
  the reference as `pkg.Type`; the resolver looks the declaration up in
  the compiled-in unit (`ModuleRegistry::FindModuleType`) and binds it,
  and an unimported package is diagnosed rather than silently bound. An
  external `.ncu` exposes no source-level types, so `pkg.Type` resolves
  only for inlined library sources.
- **Deduplication** is tracked per *fully-parsed library file*, separate
  from the symbol index's already-indexed set: standard-library
  packages are signature-indexed at build-environment construction,
  yet must still be inlined.

### Library TU vs project TU (visibility isolation)

A library TU is compiled in but is deliberately *not* a project module:

- it does not join the same-directory auto-visibility pool (the D7 rule
  is project-only), and it does not inject its directory's other files;
- wildcards never pull library TUs in as project modules;
- a library never sees the consuming project's modules; it only sees
  what it itself imports;
- one build may contain only one package of each dotted name: two roots
  offering the same package, or two units deriving one path, are a
  duplicate-package error naming both source paths (the reserved-name
  table's replacement — a project directory named `io` is an ordinary
  directory now).

Circular imports between libraries are allowed: all TUs merge before
resolution, so declarations are mutually visible within one build, the
same way several project files are.

### Run time: closure loading and linking

Generated code contains no library code, so execution begins by
gathering the import closure and linking it into one runtime module
(`src/vm/NcuLoader.cpp`, `src/vm/NcuLinker.cpp`):

1. **Loading** (nloader) — starting from the entry artifact: members of
   a `.npkg` resolve inside the package first; an import slot's target
   module is located along the search path as a `<module path>.ncu`
   file or as a member of a `.npkg` holding it, recursing until the
   closure is complete. A member image's header module path must match
   the target (a mismatch is refused), every step checks version and
   checksum, and a missing dependency is reported in one shot listing
   the searched directories.
2. **Linking** (nlink) — a pure in-memory transformation: N unit images
   are merged and deduplicated by qualified name, placeholder slots
   resolve to global table indices, every operand is remapped
   uniformly, and unresolved symbols / visibility violations are
   reported in one shot. The entry resolves from the program package's
   entry record or the bare unit's `<module path>.main` convention.

`nvm`, `ncc run`, ncc's compile-and-execute, and ndb sessions all take
this same path — there is no second load/link implementation among the
tools. At run time the standard library is simply `stdlib.npkg` on the
search path, no different from any other library package.

## 4. Search paths

The same ordered directory list serves compile-time `.n` discovery,
run-time closure-member loading (`.ncu`/`.npkg`) and native DLL loading
— first match wins, duplicates normalize
away (case-folded on Windows). Five layers, highest first
(`include/nlang/common/LibrarySearchPath.h`):

1. command-line `-I <dir>` (repeatable);
2. `.nproj` `<ImportPaths>`;
3. project / source-file / module directory;
4. `NLANG_PATH` environment variable (`;` on Windows, `:` on POSIX);
5. system defaults: stdlib directory, executable directory, current
   directory.

`ncc`, `nvm` and `ndb` all consume this one header (pure STL, no VM
dependency, so a future standalone language service reuses it at zero
coupling). nide layers it as global settings (Tools → Options) plus
per-project settings (Project → Properties), project entries first, and
passes the resulting `-I` list to build, run and debug alike. Editing a
library `.n` requires no compiler-side invalidation: every build
re-parses it from disk.

## 5. Native host ABI

Native implementations are ordinary shared libraries behind one stable
contract (`include/nlang/vm/NativeHost.h`):

- file name `nlang_<ns>.dll` (`libnlang_<ns>.so`/`.dylib`), located on
  the section-4 search path;
- exactly one exported entry point, `nlang_native_init`, registered
  through a macro that also pins the host ABI version
  (`NLANG_HOST_ABI_VERSION`); a version mismatch fails the load with a
  readable error, and a module without the entry is rejected;
- native functions receive a small **host interface** (callbacks for IO,
  PRNG and string access) instead of linking the VM — the third-party
  source needs only the one header;
- loading is lazy: the first call into a package triggers the module
  load, so unused libraries cost nothing.

Plain C++ functions in a DLL can be wrapped and combined by NLang
bodies in the same `.n` (a NLang `quad` calling a native `dbl` twice),
which is how the stdlib and `tests/fixtures/native/mylib/` work.

## 6. What the standard library is made of

There is no signature-table call path: codegen emits qualified
`OP_CallFunc`, the resolver binds against the inlined AST, and the VM
carries no standard-library knowledge.

| Artifact | Role |
|---|---|
| `stdlib/*.n` | hand-written authoritative declarations — the only signature source |
| `src/native/{math,io,fs}/` | the native implementations, built as `nlang_math.dll`, `nlang_io.dll`, `nlang_fs.dll` |
| duplicate-package check (`ModuleRegistry::RegisterUnit` + discovery) | one build may contain only one package of each dotted name; the error names both source paths |
| `kStringMethodTable` (`include/nlang/vm/StdLib.h`) | string methods only — receiver-dispatched built-ins, still intrinsics; a library package is not implemented this way |
| ctest `no_builtin_stdlib` | fails if a hardcoded table, a math/io/fs intrinsic family or their ids return, or if `stdlib/*.n` is deleted instead |

`io.print` is `native void print(string)`: int/float/array coerce through
the general string formal, while class/enum/func require an explicit
`.toString()`. There is no `TypeKind::Any` and no print-specific
argument-to-string codegen.

## 7. Decision records

- **Inline the sources at compile time for signatures, load precompiled
  library packages at run time** (phase 6's division of labor):
  inlining keeps bodies readable, modifiable and recompilable, and
  preserves one resolution path; the per-build re-parse cost of a
  handful of `.n` files is negligible until measured otherwise.
  Codegen emits only the consumer's own units; library units' images
  ship with their packages (the standard library is `stdlib.npkg`),
  and linking happens at load time — distribute-without-source and
  source inlining thereby coexist, both dispatching per function, so
  mixed native/managed libraries hold on either side.
- **No transitional dual path** (stdlib via signatures, third-party via
  AST): the inlining mechanism is identical for both, so phase 4a
  switched all libraries at once and deleted the signature-driven
  codegen outright. Behavior-preservation was carried by the full
  regression suite.
- **Generic `any` dropped rather than implemented**: implicit
  string-coercion for string formals already covered int/float/array,
  matching the rest of the language; a top type would be a second,
  special argument ABI.
- **nide does not compile third-party `.cpp` into DLLs**: the C/Python
  extension model — library authors ship the binary or a build script;
  stale-DLL detection is a warning, not an auto-build. Adding a general
  native build command would require compiler-toolchain discovery and
  cross-platform flags for little user benefit today.
- **No incremental cache for library sources**: correctness comes from
  re-parsing every build; a cache is deferred until compile-time data
  justifies it.

## 8. Remaining work (design status as of 2026-10-04)

Mechanism landed: 4a unified inlining (commit `ab4546a`), the in-process
mixed native + NLang library test (4b-1, `test_thirdparty.cpp`), the
library type surface (4b-2, section 3), the retirement of the built-in
standard library (4c, section 6), search paths (3d), native ABI and
loader (3b/3c), load-time closure loading and linking (phase 6 —
nloader/nlink, per-unit artifacts, `stdlib.npkg`; section 3 "Run
time"). Open items:

1. **4d** — change-aware recompilation: nide's standalone staleness
   check must account for inlined library `.n` files, not only the main
   source. Two candidate designs, both keeping library-discovery rules
   in the compiler alone: serialize the participating library sources
   (paths + mtimes) into the `.ncu` (format bump, no backward
   compatibility required), or a `ncc deps` query mode that reports the
   discovered list. Plus: ndb verification of breaking into library
   source, asynchronous `runNccBuild` in nide.

## 9. Testing conventions

Per the repository rules: real compilation of `.n` sources and real
execution on `VmExecutor` (no mocked internals) — see
`test_library_source.cpp`, `test_thirdparty.cpp`,
`test_native_loader.cpp`/`test_native_abi.cpp`, fixtures under
`tests/fixtures/native/`, and the `check_nvm_native.py`/`check_ndb_native.py`
e2e scripts.
