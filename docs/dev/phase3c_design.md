# Phase 3c — Third-party libraries use the same mechanism as the standard library

## Goal
A third-party library shipped as **`.n` source + a native DLL** is discovered,
resolved, code-generated and loaded through the **exact same path** as the
standard library. No hard-coded namespace list; `kStdLibTable` keeps shrinking.

## Model (mirrors Python packages / JNI)
- A library is a file `X.n` whose top level is `namespace X { ... }`.
- `native` declarations inside are implemented by `nlang_X.dll`; the `.n` file
  is the signature authority (params, return type, docs).
- The library source lives next to its DLL in one directory (a "package dir"),
  which is on the import search path (`-I`).

## Three changes

### 1. On-demand library-source discovery (compile time)
Before building import gates, for every single-segment import name not already
known:
- search the import dirs for `X.n`; if found, `LibraryIndex.LoadFileOnce(X.n)`.
`LoadFileOnce` is idempotent (a loaded-file set); only declarations inside a
`namespace` block are indexed, so ordinary program files add nothing.

### 2. Namespace recognition becomes index-driven
- `ModuleRegistry::BuildGate/ApplyImportSpec` get a predicate
  `isLibraryNamespace(name)` (injected by ModuleBuilder, backed by
  `LibraryIndex.HasNamespace`). Everywhere that hard-codes
  `IsStdLibNamespaceName` for *recognition* uses this predicate:
  - gate builtin arm, resolver member-call, reserved user-decl names, duplicate
    checks.
- A recognized library namespace opens the gate like `io` and does **not**
  require `X.nmod`.
- The remaining external names fall back to the existing `X.nmod` path.

### 3. Native runtime search path
- When running, import dirs are added to the VM's native loader search path
  (ncc compile+run / run; nvm; ndb). The executable dir stays the default.
  `nlang_X.dll` beside `X.n` is then found.

## Mixed native + nlang libraries
- This phase fully supports **native-only** third-party namespaces.
- A `.n` library that also defines nlang bodies needs those bodies compiled and
  merged, plus change-triggered recompilation — that is Phase 4. The data model
  already separates signatures; `SymbolInfo.isNative` lets codegen choose
  DLL-stub vs compiled body when Phase 4 lands. A library body that cannot yet
  be compiled gets an explicit diagnostic, never silent wrong code.

## Tests (TDD, real compile + real run, no mocks)
- New fixture `tests/fixtures/native/mylib/mylib_native.cpp` → `nlang_mylib.dll`
  (CMake SHARED).
- New suite `test_thirdparty`: write `mylib.n` + a program `import mylib` into a
  scratch package dir, compile via ModuleBuilder and run via VmExecutor with the
  package dir on the native path; assert `add/mul/greet` results and output.
- ncc end-to-end ctest: `ncc prog.n -I pkgdir` prints the expected lines.
- Full `ctest -C Release` stays green.
