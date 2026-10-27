# 22. The Native Application Binary Interface (ABI)

A *native module* is a shared library that implements native
functions — the functions declared `native` in §7.5 and called
through the qualified key `<ns>.<name>` — against the native
application binary interface (ABI). The standard library
modules and third-party modules use the same interface. §22.1
sets the scope and the ABI version; §22.2 fixes module
discovery and loading; §22.3 fixes the module exports and
registration; §22.4 fixes the calling convention and the value
cell; §22.5 defines the host function table; §22.6 defines the
pinned array view; §22.7 states the blocking-section contract.

## 22.1 Scope and Version

**ABI version.** This chapter describes ABI version **9**. The
host accepts a module whose version probe reports exactly `9`
and rejects every other value, including a module that exports
no probe at all (§22.2). An ABI change bumps the version and
replaces this chapter wholesale; the specification describes
only the current version.

**What this chapter fixes.** Module discovery and loading
(§22.2), the module exports and registration (§22.3), the
calling convention and the value cell (§22.4), the host
function table (§22.5), the pinned array view (§22.6) and the
blocking-section contract (§22.7).

**What it does not fix.** The language-side `native`
declaration, its keying and its argument rules (§7.5); the
instruction encoding inside a unit image (chapter 20); and the
embedding interface by which a host supplies a native
implementation directly instead of through a module — such a
host implementation answers before any module lookup, is
bound by the calling convention and the host function table,
and is not bound by the module exports.

**Statelessness.** A native module keeps no state the language
can observe: the pseudorandom number generator (PRNG) and the
console live in the host and are reachable only through the
host function table (§22.5).

## 22.2 Module Discovery and Loading

**Module file.** The module that implements namespace `<ns>`
is a shared library named `nlang_<ns>.dll` on Windows,
`libnlang_<ns>.dylib` on macOS and `libnlang_<ns>.so` on other
POSIX systems.

**Lazy loading.** On the first call whose key `<ns>.<name>` is
not already registered, the host loads the module for the
namespace — the segment before the first dot (§7.5). A key
with no dot is a bare name and never triggers a module load.
Loading happens once per namespace; every later call in the
same namespace reuses the loaded module.

**Search order.** The host searches an ordered list of
directories for the module file. The directory of the running
executable is always in the list; an embedding host may add
directories, and a development build appends the build-tree
directory that holds its own native outputs. A module found in
no directory raises a host-level run-time error at the call
(§9.1) naming the file and every directory consulted.

**Load sequence.** The host opens the module file and then:

1. resolves the export `nlang_abi_version`; a module without
   the export is rejected;
2. calls it; a value other than `9` is rejected;
3. resolves the export `nlang_native_init`; a module without
   the export is rejected;
4. calls it once (§22.3).

A rejection at step 1 or 2 unloads the module without running
any of its code: the registrations of an incompatible module —
pointers into a library the host unloads at once — must never
enter the native table. A rejected module raises a host-level
run-time error at the call (§9.1).

**Lifetime.** A loaded module stays loaded until the host
shuts down, and modules are unloaded in reverse order of
loading.

## 22.3 Exports and Registration

**Exports.** The ABI surface of a module is two symbols with C
linkage, unmangled on every platform the ABI supports:

| Export | Signature | Contract |
|---|---|---|
| `nlang_abi_version` | `unsigned (void)` | returns `9`; the single version authority (§22.2) |
| `nlang_native_init` | `int (void* registry, RegisterNativeFn reg, RegisterNativeBlockingFn regBlocking)` | called once after the probe passes |

with the registration callbacks

```c
void reg        (void* registry, const char* ns,
                 const char* name, NativeFn fn);
void regBlocking(void* registry, const char* ns,
                 const char* name, NativeFn fn);
```

**Registration.** Inside `nlang_native_init` the module
registers each of its functions through `reg` — a non-blocking
native, called while the global lock is held (§11.1) — or
through `regBlocking` — a native with a blocking section
(§22.7) — passing the opaque `registry` pointer through
unchanged. The registered key is `<ns>.<name>`. The return
value of `nlang_native_init` is informational; the probe is
the single version authority.

**Header.** The whole interface — the version constant, the
function types, the host table and the export macros — is
defined by the public header `nlang/vm/NativeHost.h`. A
module includes it and writes its init body with the provided
macro:

```c
NLANG_DEFINE_NATIVE_INIT {
    reg(registry, "math", "sqrt", &mathSqrt);
    regBlocking(registry, "fs", "readFile", &fsReadFile);
}
```

The macro emits the `nlang_abi_version` probe automatically,
so a module cannot drift from the header's version constant.

## 22.4 Calling Convention and Value Cell

**Signature.** A native function has the signature

```c
void fn(NativeHost* host, uint8_t* ret,
        const uint8_t* args, int argc);
```

- `host` — the host function table (§22.5);
- `ret` — one value cell that receives the return value; a
  `void` native writes nothing;
- `args` — the base of `argc` value cells holding the
  arguments;
- `argc` — the declared parameter count of the call.

**Arguments.** A free native function receives its first
formal in `args[0]`. A method or constructor receives the
receiver in `args[0]` and its formals after it (§7.5).
Default arguments are filled at the call site before
dispatch, so `argc` always equals the declared count (§7.5).
Native functions have no `out` parameters (§7.5).

**Value cell.** Every argument, the receiver and the return
value travel in one eight-byte cell:

| Content | Encoding |
|---|---|
| `int`, `short`, `byte`, `bool`, `char` and their unsigned forms | the value in the low four bytes |
| `float` | the IEEE 754 binary32 bit pattern in the low four bytes |
| `long`, `double` | the value or bit pattern in all eight bytes |
| `string`, a class or struct instance, an array, `List`, `Dict`, a function value | a 32-bit heap handle in the low four bytes |

The other four bytes of a cell that carries a four-byte value
or a handle are never read. A handle is the same identifier
the garbage collector traces (§10.2): a native passes it back
through the host table and never dereferences it directly.

**Frame discipline.** The native reads `args` and writes `ret`
and nothing else; no frame memory beyond the cells is visible
to it, and nothing obtained through the host table survives
the call (§22.5, §22.6).

## 22.5 The Host Function Table

**Layout.** `NativeHost` is a struct of function pointers
whose first field is `abiVersion`, a `uint32_t` holding `9`,
followed in fixed order by the fifteen callbacks below. Each
callback takes the table itself as its first parameter, shown
as `self` in the signatures:

| Callback | Signature |
|---|---|
| `getString` | `const char* (NativeHost* self, int32_t handle)` |
| `newString` | `int32_t (NativeHost* self, const char* utf8)` |
| `newListString` | `int32_t (NativeHost* self, const char* const* items, int count)` |
| `writeOutput` | `void (NativeHost* self, const char* text)` |
| `writeError` | `void (NativeHost* self, const char* text)` |
| `readLine` | `const char* (NativeHost* self)` |
| `readToken` | `const char* (NativeHost* self)` |
| `readChar` | `int (NativeHost* self, uint32_t* outChar)` |
| `hasInput` | `int (NativeHost* self)` |
| `raiseException` | `void (NativeHost* self, int exceptionKind, const char* message)` |
| `nextRandom` | `uint32_t (NativeHost* self)` |
| `seedRandom` | `void (NativeHost* self, int32_t seed)` |
| `getArrayView` | `int (NativeHost* self, int32_t handle, NativeArrayView* out)` |
| `newArray` | `int32_t (NativeHost* self, uint8_t elemKind, int32_t length)` |
| `newArrayShaped` | `int32_t (NativeHost* self, uint8_t elemKind, int32_t rank, const int32_t* dims)` |

**Strings.** `getString` returns the bytes of the string
behind `handle`; `newString` creates a string from a
NUL-terminated UTF-8 (Unicode Transformation Format) buffer
and returns its handle; `newListString` creates a
`List<string>` from `count` NUL-terminated buffers in one
call. The buffer `getString` returns is valid only until the
next host callback — the native copies it immediately.

**Console.** `writeOutput` writes text verbatim to standard
output (§15.2); `writeError` writes it to the diagnostic
channel — standard error, or the session's single merged
output view when an embedding host owns the streams (§15.2).
`readLine` returns one input line with the trailing newline
stripped, or null at end of input; an empty line is `""`
(§15.3). `readToken` skips whitespace across lines and
returns the next token; at end of input the host raises
before returning, so a null result is defensive only (§15.3).
`readChar` decodes one full UTF-8 scalar into `outChar` and
returns `0`, or returns `1` after raising at end of input;
text that is not valid UTF-8 raises the base `Exception`
(§15.3). `hasInput` returns `1` when a `readLine` would still
produce a line and never raises (§15.3). The buffers the
input callbacks return are valid only until the next host
callback.

**Exceptions.** `raiseException` raises an exception of the
class selected by `exceptionKind` and never returns:

| `exceptionKind` | Raised class |
|---|---|
| `0` | `Exception` |
| `1` | `IOException` |
| `2` | `NullPointerException` |
| `3` | `IndexOutOfBoundsException` |
| `4` | `DivByZeroException` |

The raised object is an ordinary exception of §9.3 with
`message` as its message: NLang code catches it exactly as it
catches a `throw` (§9.1).

**Random.** `nextRandom` draws the next value and `seedRandom`
seeds the generator; the state belongs to the host and is
shared by every native in the run.

**Arrays.** `newArray` mints a one-dimensional array and
`newArrayShaped` an array of the given rank (§4.3), both
zero-initialized, and return the handle. The element kind is
one of four codes, which mirror the type-kind codes of §20.5:

| Code | Element type |
|---|---|
| `0` | `int` |
| `1` | `float` |
| `15` | `long` |
| `17` | `double` |

A kind outside these four raises the base `Exception`. A
negative length or dimension, and any request past the
allocation guard, raises `IndexOutOfBoundsException` — the
same outcome as a negative sized creation on the language side
(§4.3, §9.3). A rank outside `1` to `64` raises the base
`Exception` (§4.3). The minted handle is kept alive by the
host until the native writes it into `ret` (§22.7).

`getArrayView` fills `*out` with the pinned view of §22.6 of
the array behind `handle` and returns `1`; a handle that is
null or out of range raises `NullPointerException`, and a
handle of a non-array value raises the base `Exception` —
there is no non-raising failure.

## 22.6 Pinned Array Views

**View.** `NativeArrayView` describes one heap array's
payload:

| Field | Type | Content |
|---|---|---|
| `rank` | `int32_t` | the array's rank (§4.3) |
| `dims` | `const int32_t*` | the dimension lengths; points into the array record |
| `strides` | `const int32_t*` | the row-major strides in elements; points into the view itself |
| `elemCount` | `int64_t` | the total element count |
| `elemKind` | `uint8_t` | the element kind (§22.5) |
| `elemCellBytes` | `uint8_t` | the per-element stride in bytes: `4`, or `8` for `long` and `double` |
| `data` | `void*` | the payload base — element zero |

**Addressing.** The element at indices `i[0] … i[rank-1]`
lies at

```
data + (i[0]*strides[0] + … + i[rank-1]*strides[rank-1])
     * elemCellBytes
```

**Lifetime.** The garbage collector does not move objects and
the argument record is kept alive for the duration of the
native call, so every pointer of the view is stable for the
call — and only for the call. Never cache a view past the
return.

**Owned strides.** `strides` points into the view itself, and
each view owns its strides: copying the struct leaves the
copy's `strides` pointing into the original. Request a fresh
view instead of copying one.

## 22.7 Blocking Sections

**The two channels.** A native registered through `reg` runs
while the global lock is held: it must not wait on anything
outside the virtual machine (§11.1). A native registered
through `regBlocking` declares a blocking section: it may
take unbounded time waiting on the outside world, and the
virtual machine releases the global lock around its call so
other contexts run meanwhile (§11.1).

**Staging.** The blocking section is transparent to the
signature. Before releasing the lock the host copies the
argument cells and the result cell onto C++ stack storage,
keeps them reachable by the collector, and parks the calling
context; after the native returns it copies the result back
and resumes the context. The native sees the same `args` and
`ret` either way.

**Callbacks.** Every host callback reacquires the global lock
internally, so a blocking native may call any callback at any
point of its section; the native itself must not assume
exclusive access to the virtual machine inside the section.

**Rooting.** A handle minted inside a blocking section
(§22.5) is kept alive by the host until the native writes it
into `ret`; a minted handle that is never published may be
collected once the section ends.

**Propagation.** An exception raised inside a blocking section
— by `raiseException` or by a host callback — is held until
the calling context is restored, and then propagates to the
caller's handler like any other raise (§9.1).
