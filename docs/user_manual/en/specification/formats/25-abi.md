# 25. The Native Application Binary Interface (ABI)

A *native module* is a shared library that implements native
functions — the functions declared `native` in §7.5 and called
through the qualified key `<ns>.<name>` — against the native
application binary interface (ABI). The standard library
modules and third-party modules use the same interface. §25.1
sets the scope and the ABI version; §25.2 fixes module
discovery and loading; §25.3 fixes the module exports and
registration; §25.4 fixes the calling convention and the value
cell; §25.5 defines the host function table; §25.6 defines the
pinned array view; §25.7 states the blocking-section contract
and the input/output (IO) parking built on it.

## 25.1 Scope and Version

**ABI version.** This chapter describes ABI version **11**. The
host accepts a module whose version probe reports exactly `11`
and rejects every other value, including a module that exports
no probe at all (§25.2). An ABI change bumps the version and
replaces this chapter wholesale; the specification describes
only the current version.

**What this chapter fixes.** Module discovery and loading
(§25.2), the module exports and registration (§25.3), the
calling convention and the value cell (§25.4), the host
function table (§25.5), the pinned array view (§25.6) and the
blocking-section contract and the IO parking built on it
(§25.7).

**What it does not fix.** The language-side `native`
declaration, its keying and its argument rules (§7.5); the
instruction encoding inside a unit image (chapter 23); and the
embedding interface by which a host supplies a native
implementation directly instead of through a module — such a
host implementation answers before any module lookup, is
bound by the calling convention and the host function table,
and is not bound by the module exports.

**Statelessness.** A native module keeps no state the language
can observe: the pseudorandom number generator (PRNG) and the
console live in the host and are reachable only through the
host function table (§25.5).

## 25.2 Module Discovery and Loading

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
(§8.1) naming the file and every directory consulted.

**Load sequence.** The host opens the module file and then:

1. resolves the export `nlang_abi_version`; a module without
   the export is rejected;
2. calls it; a value other than `10` is rejected;
3. resolves the export `nlang_native_init`; a module without
   the export is rejected;
4. calls it once (§25.3).

A rejection at any step unloads the module before its init
runs: the registrations of an incompatible module — pointers
into a library the host unloads at once — must never enter the
native table. A rejected module raises a host-level run-time
error at the call (§8.1).

**Lifetime.** A loaded module stays loaded until the host
shuts down, and modules are unloaded in reverse order of
loading.

## 25.3 Exports and Registration

**Exports.** The ABI surface of a module is two symbols with C
linkage, unmangled on every platform the ABI supports:

| Export | Signature | Contract |
|---|---|---|
| `nlang_abi_version` | `unsigned (void)` | returns `11`; the single version authority (§25.2) |
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
native, which runs with no lock held (§10.1) — or through
`regBlocking` — a native with a blocking section (§25.7) —
passing the opaque `registry` pointer through unchanged. The registered key is `<ns>.<name>`. The return
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

## 25.4 Calling Convention and Value Cell

**Signature.** A native function has the signature

```c
void fn(NativeHost* host, uint8_t* ret,
        const uint8_t* args, int argc);
```

- `host` — the host function table (§25.5);
- `ret` — one value cell that receives the return value; a
  `void` native writes nothing;
- `args` — the base of `argc` value cells holding the
  arguments;
- `argc` — the number of value cells in `args`: the declared
  formal count for a free function, one more for a method or
  constructor, counting the receiver (Arguments, below).

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
the garbage collector traces (§9.2): a native passes it back
through the host table and never dereferences it directly.

**Frame discipline.** The native reads `args` and writes `ret`
and nothing else; no frame memory beyond the cells is visible
to it, and nothing obtained through the host table survives
the call (§25.5, §25.6).

## 25.5 The Host Function Table

**Layout.** `NativeHost` is a struct of function pointers
whose first field is `abiVersion`, a `uint32_t` holding `11`,
followed in fixed order by the twenty-two callbacks below. Each
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
| `newArrayView` | `int32_t (NativeHost* self, int32_t targetHandle, int32_t rank, const int32_t* dims, const int32_t* strides, int64_t offset)` |
| `nowMs` | `long long (NativeHost* self)` |
| `parkForIo` | `int (NativeHost* self, int fd, unsigned events, long long deadlineMs, int hasDeadline)` |
| `moduleState` | `void* (NativeHost* self, const char* tag, void* (*ctor)(void), void (*dtor)(void*))` |
| `wakeIoWaiters` | `void (NativeHost* self, int fd)` |
| `currentContextId` | `int (NativeHost* self)` |
| `submitWork` | `int (NativeHost* self, void (*fn)(void*), void* arg, int ackFd, int priority)` |

**Strings.** `getString` returns the bytes of the string
behind `handle`; `newString` creates a string from a
NUL-terminated UTF-8 (Unicode Transformation Format) buffer
and returns its handle; `newListString` creates a
`List<string>` from `count` NUL-terminated buffers in one
call. The buffer `getString` returns is valid only until the
next host callback — the native copies it immediately.

**Console.** `writeOutput` writes text verbatim to standard
output (§14.2); `writeError` writes it to the diagnostic
channel — standard error, or the session's single merged
output view when an embedding host owns the streams (§14.2).
`readLine` returns one input line with the trailing newline
stripped, or null at end of input; an empty line is `""`
(§14.3). `readToken` skips whitespace across lines and
returns the next token; at end of input the host raises
before returning, so a null result is defensive only (§14.3).
`readChar` decodes one full UTF-8 scalar into `outChar` and
returns `0`; at end of input the host raises before returning,
so the `1` result is defensive only (§14.3). Text that is not
valid UTF-8 raises the base `Exception` (§14.3). `hasInput`
returns `1` when a `readLine` would still produce a line and
never raises (§14.3). The buffers the
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

The raised object is an ordinary exception of §8.3 with
`message` as its message: NLang code catches it exactly as it
catches a `throw` (§8.1).

**Random.** `nextRandom` draws the next value and `seedRandom`
seeds the generator; the state belongs to the host and is
shared by every native in the run.

**Arrays.** `newArray` mints a one-dimensional array and
`newArrayShaped` an array of the given rank (§4.10), both
zero-initialized, and return the handle. The element kind is
one of six codes, which mirror the type-kind codes of §23.5:

| Code | Element type |
|---|---|
| `0` | `int` |
| `1` | `float` |
| `10` | `byte` |
| `11` | `ubyte` |
| `15` | `long` |
| `17` | `double` |

A kind outside these six raises the base `Exception`. A
negative length or dimension, and any request past the
allocation guard, raises `IndexOutOfBoundsException` — the
same outcome as a negative sized creation on the language side
(§4.10, §8.3). A rank outside `1` to `64` raises the base
`Exception` (§4.10). The minted handle is kept alive by the
host until the native writes it into `ret` (§25.7).

`newArrayView` mints a view (§4.10) over the array — or view —
behind `targetHandle`, with the given rank, dims, strides and
offset (element units, §25.6); the element kind comes from the
target. The validation is the one the language's own view
creation applies: a null or non-array target raises
`NullPointerException` or `IndexOutOfBoundsException`
respectively, a rank outside `1` to `64` or a non-scalar
element kind raises the base `Exception`, and bad dims or a
range exceeding the target's bounds raise
`IndexOutOfBoundsException` (§4.10). The minted handle is kept
alive by the host until the native writes it into `ret`
(§25.7).

`getArrayView` fills `*out` with the pinned view of §25.6 of
the array — owning record or view record — behind `handle` and
returns `1`; a handle that is null or out of range raises
`NullPointerException`, and a handle of a non-array value
raises the base `Exception` — there is no non-raising failure.

**Byte cells.** `byte` and `ubyte` arrays use the narrow-cell
convention: one `int32_t` cell per element holding the value
in its low byte. A view of such an array reports
`elemCellBytes` `4` like an `int` array; the addressing of
§25.6 is unchanged.

**IO parking and the work pool.** `nowMs` returns monotonic
milliseconds — the clock domain every park deadline in this
section uses (§10.1); it is not wall time. `parkForIo`
registers interest in `fd` for `events` — `NLANG_EV_READ` `1`,
`NLANG_EV_WRITE` `2` — plus an optional absolute deadline, and
unwinds the caller as §25.7 describes. `wakeIoWaiters`
unregisters the poller's interest in `fd` and wakes every
context parked on it — the socket-close path; it takes no
lock of its own and may be called with none held.
`currentContextId` returns the calling context's id, stable
across park and re-entry (§10.1) — the identity a native uses
to tell its own re-entry from another context's concurrent
call. It is callable only from a blocking-section native.
`submitWork` runs `fn(arg)` on the host's generic native
worker pool, whose workers hold no lock; on completion the
pool writes one byte to `ackFd`, a file descriptor the caller
owns (`-1` asks for none). `priority` `0` is normal and `1`
high — a two-level queue, so a slow lookup never starves
short jobs. The pool starts lazily and is joined when the
scheduler shuts down; `submitWork` itself is callable from
either channel and returns `0`.

**Module state.** `moduleState` returns the host-managed
per-executor state slot named `tag`: the host calls `ctor`
once per executor, stores the result with `dtor`, and hands
the same pointer to every later call; `dtor` runs at executor
teardown, before the module would unload (modules never
unload). State stored this way is never visible to another
executor — the isolation the `net` package's socket table
relies on.

## 25.6 Pinned Array Views

**View.** `NativeArrayView` describes one heap array's
payload — an owning record's, or a view record's window into
its owning record:

| Field | Type | Content |
|---|---|---|
| `rank` | `int32_t` | the array's rank (§4.10) |
| `dims` | `const int32_t*` | the dimension lengths; points into the array record |
| `strides` | `const int32_t*` | the strides in elements; points into the view itself |
| `elemCount` | `int64_t` | the total element count |
| `elemKind` | `uint8_t` | the element kind (§25.5) |
| `elemCellBytes` | `uint8_t` | the per-element stride in bytes: `4`, or `8` for `long` and `double` |
| `reserved` | `uint8_t[2]` | written `0`; the reader ignores the field |
| `offset` | `int64_t` | where element zero lives, in elements (§25.6 addressing) |
| `stridesInline` | `int32_t[64]` | the storage `strides` points at: 64 entries, the rank ceiling (§25.5) |
| `data` | `void*` | the payload base of the owning record |

**Addressing.** The element at indices `i[0] … i[rank-1]`
lies at

```
data + (offset + i[0]*strides[0] + … + i[rank-1]*strides[rank-1])
     * elemCellBytes
```

`offset` is in element units, the same unit as `strides`. An
owning array reports `0` and row-major strides computed from
its dims; a view reports its recorded offset and strides into
the owning payload, with `data` still pointing at the owning
record's payload base.

**Lifetime.** The garbage collector does not move objects and
the argument record is kept alive for the duration of the
native call, so every pointer of the view is stable for the
call — and only for the call. Never cache a view past the
return.

**Owned strides.** `strides` points into the view itself, and
each view owns its strides: copying the struct leaves the
copy's `strides` pointing into the original. Request a fresh
view instead of copying one.

## 25.7 Blocking Sections

**The two channels.** A native registered through `reg` runs
with no lock held: it must not wait on anything outside the
virtual machine, and its stores take the category locks
through self-guarded callbacks (§10.1). A native registered
through `regBlocking` declares a blocking section: it may
take unbounded time waiting on the outside world, and the
virtual machine releases the locks it holds around its call
so other contexts run meanwhile (§10.1).

**Staging.** The blocking section is transparent to the
signature. Before releasing those locks the host copies the
argument cells and the result cell onto C++ stack storage,
keeps them reachable by the collector, and parks the calling
context; after the native returns it copies the result back
and resumes the context. The native sees the same `args` and
`ret` either way.

**Parking.** A blocking native that would wait on a file
descriptor parks instead of blocking its carrier: it calls
`parkForIo` (§25.5) with the descriptor, the event bits it
waits for, and an optional absolute deadline in `nowMs`
domain. On the first entry `parkForIo` returns `0` — the
interest is registered and the native must return immediately
without touching anything else; the host parks the calling
context and releases the carrier. When the descriptor is ready
or the deadline passes, the host re-executes the same native
call with the same staged arguments, and `parkForIo` returns
`1` — the native retries the operation and either completes or
parks again. A descriptor below zero parks on the deadline
alone. Because a wake may arrive without the awaited
condition holding — another context's close, a stale wake — a
native re-checks its condition on every return and treats
`parkForIo` as a retry hint, never as a guarantee. Parking
exists only for natives the language dispatches: the host
re-executes the call instruction that invoked the native, so a
native invoked by the host itself — the native-main root
window, a host bridge — has no such anchor and must not park.

**Callbacks.** Every host callback guards itself — it takes
the lock its store needs and releases it before returning —
so a blocking native may call any callback at any point of
its section; the native itself must not assume exclusive
access to the virtual machine inside the section.

**Rooting.** A handle minted inside a blocking section
(§25.5) is kept alive by the host until the native writes it
into `ret`; a minted handle that is never published may be
collected once the section ends.

**Propagation.** An exception raised inside a blocking section
— by `raiseException` or by a host callback — is held until
the calling context is restored, and then propagates to the
caller's handler like any other raise (§8.1).
