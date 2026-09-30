# Phase 3b — Native dynamic loading: unify stdlib and third-party libraries

## Goal

Move the math/io/fs implementations out of the hardcoded `ExecuteIntrinsic`
switch into dynamically-loaded native modules (`nlang_math.dll`,
`nlang_io.dll`, `nlang_fs.dll`). Standard library and third-party libraries
then use the **same** discovery / signature / native-loading mechanism. After
this phase `kStdLibTable` is deleted. Future LSP is unaffected (signatures
still come from `stdlib/*.n` via `langservice::SymbolIndex`).

## Reference models

- Java JNI: `JNIEnv*` function table passed into every native call; state
  lives in the VM, native code is a stateless bridge.
- Python C extensions / Lua C API: host callbacks for object creation.
- SQLite extensions: module entry registers functions through a host
  callback; an explicit `void*` registry pointer avoids global state.
- C# P/Invoke: flat C ABI, `DllImport` names an external symbol.

Decision: a C ABI function table (`NativeHost`) is passed per call; native
state that must persist (the PRNG) stays in the host. A module's
`nlang_native_init` registers its functions through a host callback with an
explicit registry pointer. This keeps DLLs stateless, ABI-stable and usable
from C.

## Native ABI (include/nlang/vm/NativeHost.h)

```c
#define NLANG_HOST_ABI_VERSION 1

typedef struct NativeHost NativeHost;
struct NativeHost {
    uint32_t abiVersion;
    const char* (*getString)(NativeHost* self, int32_t handle);
    int32_t     (*newString)(NativeHost* self, const char* utf8);
    int32_t     (*newListString)(NativeHost* self,
                                 const char* const* items, int count);
    void         (*writeOutput)(NativeHost* self, const char* text);
    const char* (*readLine)(NativeHost* self);
    void         (*raiseException)(NativeHost* self, int kind,
                                   const char* message); /*noreturn*/
    uint32_t     (*nextRandom)(NativeHost* self);
    void         (*seedRandom)(NativeHost* self, int32_t seed);
};

/* exception class selector */
enum NativeExceptionKind { NEXC_Base = 0, NEXC_IOException = 1 };

typedef void (*NativeFn)(NativeHost* host, uint8_t* ret,
                         const uint8_t* args, int argc);

/* module entry */
typedef void (*RegisterNativeFn)(void* registry, const char* ns,
                                 const char* name, NativeFn fn);
typedef int  (*NativeModuleInitFn)(void* registry, RegisterNativeFn reg);
/* exported symbol: nlang_native_init ; returns NLANG_HOST_ABI_VERSION */
```

Returned `const char*` is valid only until the next host callback; C++
inline wrappers copy it immediately into `std::string`.

C++ inline convenience wrappers (same header, C++ section):
- `NativeArgString(host,args,slot) -> std::string`
- `NativeReturnString(host,ret,std::string)`
- `NativeReturnInt/...` ; `NativeArgInt/Float`
- `NativeRaise(host,kind,msg)`

## Library search path (unified)

One concept — **library directories**: each may contain both `*.n`
declarations and `nlang_*.dll` modules.

- Compile time: `BuildParams.m_libraryDirs` (replaces the single
  `m_sStdLibDir`). ncc prepends the located stdlib dir; nide adds
  user-configured dirs. `BuildEnvironment` loads every dir into the
  SymbolIndex.
- Run time: `VmExecutor` NativeLibraryLoader searches the executable dir
  plus every library dir for `nlang_<ns>.dll` / `libnlang_<ns>.so`.
- Reserved-namespace checks become data-driven:
  `BuildEnvironment::IsLibraryNamespace(name)` queries the SymbolIndex
  (replaces hardcoded `IsStdLibNamespaceName`); third-party namespaces are
  protected automatically.
- Loading is lazy: the first call into namespace `ns` loads `nlang_<ns>`.

## Deploy layout

- Release prefix: `bin/` (tools + `nlang_*.dll` + Qt), `stdlib/` (the .n
  declarations). `FindStdLibDir` already resolves `bin -> ../stdlib`.
- Third-party: drop `mylib.n` + `nlang_mylib.dll` in one configured
  library directory.
- Add install rules for `stdlib/` and the native DLLs.

## TDD steps (each independently buildable/verifiable)

- **Step A** NativeHost.h ABI + C++ wrappers; compile + a mock-host unit
  test of the wrappers.
- **Step B** NativeLibraryLoader (cross-platform dlopen, search, init,
  registry trampoline, handle ownership, ExecutableDir); unit test using a
  real fixture DLL; missing DLL / bad ABI / missing entry errors.
- **Step C** VmExecutor integration: implement the NativeHost callbacks
  (reuse StrValCopy/MintNewString; extract List<string> construction from
  IntrinsicsFs; IO via IHostIo/std streams; PRNG via m_rng); rework
  CallNative/RegisterNative to the new ABI; migrate TestNatives. Verified
  with a fixture native DLL (real compile + run), before touching stdlib.
- **Step D** Move math/io/fs implementations into three SHARED modules
  under `src/native/`; build + copy beside the tools; install rules.
  End-to-end run of io/math/fs programs through the new DLLs (the old
  intrinsic path still exists until F, so no regression mid-way).
- **Step E** Codegen switch: EmitStdLibCall synthesizes an isNative
  CompiledFunction stub (name `ns.name`, paramCount/localsSize from the
  signature) and emits OP_CallFunc instead of OP_CallIntrinsic;
  EmitExprMemberCall follows; reserved-namespace checks become
  index-driven. Add a third-party native e2e (fixture `extest` DLL + .n).
- **Step F** Cleanup: delete kStdLibTable/StdLibEntry/FindStdLibFunction,
  the math/io/fs intrinsic id blocks, IntrinsicsMath/Io/Fs.cpp and the
  ExecuteIntrinsic branches/static_asserts; rework the consistency script
  (stdlib .n native decls <-> registerFn calls in src/native); migrate
  test_stdlib/test_debugger/test_module_import; nide search-path settings
  UI (SettingsStore + SettingsDialog, applied to compile + run); docs.
  Full ctest green; commit (English Conventional Commits).

## Untouched in this phase

- `kStringMethodTable` / IntrinsicsString.cpp / `INTR_String_*` (receiver
  string methods) and every other intrinsic family (ByteStream/FileStream/
  Object/List/Dict/Exception). `SLRT_*` stays for the string table.

## Acceptance

- Full ctest passes (no count regression).
- A program using io/math/fs compiles and runs identically to before.
- A fixture third-party native library works through the same path.
- Native failures raise the correct nlang exception class.
- stdlib/*.n remain the signature authority; F12/hover/completion work.
