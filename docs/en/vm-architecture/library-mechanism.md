# Library Mechanism: Search Paths, Source Inlining and Native Modules

This page records the design of NLang's library system as it stands after
the 0.7.4 rounds (phases 1–4c): how a library is found, parsed, compiled
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

Inside `<name>.n`, a `namespace <name>` block carries the library's
surface. Functions come in two kinds, freely mixed (a *mixed library*):

- **plain NLang functions** — have bodies; compiled from source into the
  consumer's module and executed as bytecode;
- **`native` functions** — signature plus documentation comment only (no
  body); at run time dispatched through the host ABI into
  `nlang_<name>.dll` (section 5).

The standard library follows the same shape: `stdlib/io.n`, `math.n`,
`fs.n` are hand-written authoritative declaration files (the io/math/fs
namespaces are pure native today), built as `nlang_math.dll`,
`nlang_io.dll`, `nlang_fs.dll` from `src/native/`.

## 3. Compilation model: full source inlining

Library `.n` files reached through a single-segment `import` are parsed
**completely** — bodies included — as *library translation units* and
merged into the same AST root as the project's units
(`src/compiler/ModuleBuilderImports.cpp`,
`src/compiler/builder/ModuleRegistry*`). Build order:

1. parse project sources (collecting imports);
2. **library discovery to a fixpoint**: a worklist seeded with the
   project's single-segment imports; each located `<name>.n` is (a)
   signature-indexed (`langservice::SymbolIndex`) and (b) fully parsed as
   a library TU, whose own single-segment imports join the worklist —
   so a third-party library may depend on other libraries;
3. register all TUs; library TUs carry an `isLibrary` flag;
4. expand aliases, build import gates, load `.nmod` externals;
5. merge everything, resolve, emit.

Consequences of one shared model:

- **Namespace-qualified calls compile as ordinary calls.** A call
  `mylib.f(...)` emits `OP_CallFunc` with the fully-qualified name
  `ns.f`; run time dispatches on the callee's own nature — a bytecode
  body, or `isNative` → the DLL. There is no signature-table call path
  and no per-library codegen branch anymore.
- **Top-level function names in the function table are fully
  qualified** for namespaced functions (including library natives), so
  the runtime lookup `m_natives["ns.name"]` matches the DLL's exported
  registrations.
- **Types defined by a library are usable**: namespaces merge into the
  root, so a library's class/struct/enum/interface resolve at the
  consumer side like project types (inheritance, virtual dispatch and
  enum methods included). A type position writes the reference as
  `ns.Type`; the resolver looks the declaration up inside the compiled-in
  unit (`ModuleRegistry::FindModuleType`) and binds it, and an
  unimported namespace is diagnosed rather than silently bound. An
  external `.nmod` exposes no source-level types, so `ns.Type` resolves
  only for inlined library sources.
- **Deduplication** is tracked per *fully-parsed library file*, separate
  from the symbol index's already-indexed set: standard-library
  namespaces are signature-indexed at build-environment construction,
  yet must still be inlined.

### Library TU vs project TU (visibility isolation)

A library TU is compiled in but is deliberately *not* a project module:

- it does not join the same-directory auto-visibility pool (the D7 rule
  is project-only), and it does not inject its directory's other files;
- wildcards never pull library TUs in as project modules;
- a library never sees the consuming project's modules; it only sees
  what it itself imports;
- the reserved segments `io`/`math`/`fs` stay blocked as project module
  directory names (`FindReservedSegment`), so a project cannot shadow a
  standard library today (project-side shadowing is a future decision).

Circular imports between libraries are allowed: all TUs merge before
resolution, so declarations are mutually visible within one build, the
same way several project files are.

## 4. Search paths

The same ordered directory list serves compile-time `.n` discovery and
run-time native DLL loading — first match wins, duplicates normalize
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
- loading is lazy: the first call into a namespace triggers the module
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
| `IsReservedLibraryName` (`src/compiler/builder/ModuleRegistry.h`) | the reserved names `io`/`math`/`fs`, blocking a project directory from shadowing a standard library |
| `kStringMethodTable` (`include/nlang/vm/StdLib.h`) | string methods only — receiver-dispatched built-ins, still intrinsics; a library namespace is not implemented this way |
| ctest `no_builtin_stdlib` | fails if a hardcoded table, a math/io/fs intrinsic family or their ids return, or if `stdlib/*.n` is deleted instead |

`io.print` is `native void print(string)`: int/float/array coerce through
the general string formal, while class/enum/func require an explicit
`.toString()`. There is no `TypeKind::Any` and no print-specific
argument-to-string codegen.

## 7. Decision records

- **Inline the source instead of precompiling libraries to `.nmod`**
  (the earlier plan's performance mitigation): inlining is what makes
  bodies readable, modifiable and recompilable, keeps one resolution
  path, and the per-build re-parse cost of a handful of `.n` files is
  negligible until measured otherwise. `.nmod` remains the
  distribute-without-source format; both forms dispatch per function,
  so mixed native/managed libraries work in either.
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

## 8. Remaining work (design status as of 2026-09-29)

Mechanism landed: 4a unified inlining (commit `ab4546a`), the in-process
mixed native + NLang library test (4b-1, `test_thirdparty.cpp`), the
library type surface (4b-2, section 3), the retirement of the built-in
standard library (4c, section 6), search paths (3d), native ABI and
loader (3b/3c). Open items:

1. **4d** — change-aware recompilation: nide's standalone staleness
   check must account for inlined library `.n` files, not only the main
   source. Two candidate designs, both keeping library-discovery rules
   in the compiler alone: serialize the participating library sources
   (paths + mtimes) into the `.nmod` (format bump, no backward
   compatibility required), or a `ncc deps` query mode that reports the
   discovered list. Plus: ndb verification of breaking into library
   source, asynchronous `runNccBuild` in nide, documentation and
   translations, VERSION/CHANGELOG entry.

## 9. Testing conventions

Per the repository rules: real compilation of `.n` sources and real
execution on `VmExecutor` (no mocked internals) — see
`test_library_source.cpp`, `test_thirdparty.cpp`,
`test_native_loader.cpp`/`test_native_abi.cpp`, fixtures under
`tests/fixtures/native/`, and the `check_nvm_native.py`/`check_ndb_native.py`
e2e scripts.
