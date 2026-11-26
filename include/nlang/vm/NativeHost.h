#pragma once

// NativeHost — the stable C ABI between the NLang virtual machine and
// dynamically-loaded native modules (standard library AND third-party
// libraries use the exact same mechanism).
//
// Design (modeled on JNI's JNIEnv, the Lua/Python C APIs and SQLite's
// extension entry points):
//   * A function table (NativeHost) is passed into every native call. The
//     native code never touches VM internals — it reads arguments and
//     creates values only through the host callbacks.
//   * Native modules are stateless. Mutable state that must follow the VM
//     (e.g. the PRNG, which the VM re-seeds per run) lives in the host.
//   * A module exports `nlang_native_init`; inside it registers each
//     function through a host callback, passing back the opaque registry
//     pointer. No global/thread-local registration state is required.
//
// The core declarations below are C-compatible. The C++ convenience
// wrappers live in the `#ifdef __cplusplus` section at the end.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bump on any incompatible change to NativeHost, NativeFn or the value
// layout. A module's nlang_abi_version export reports this; the host
// resolves and validates that probe BEFORE the module's init runs, so a
// version mismatch (or a module without the probe) is rejected without
// calling nlang_native_init. Version 2: the value slot widened from 4 to 8 bytes (the
// 0.7.5 slot-width ABI — one uniform frame cell per value). Version 3:
// the writeError callback added (io.eprint's stderr channel — only the
// host knows whether a session owns the streams). Version 4: the input
// side — readToken/readChar/hasInput added and readLine's null-at-end-
// of-input contract made explicit. Version 5: the registration side
// gained a blocking channel — nlang_native_init takes a second
// registrar, regBlocking, for natives that may take unbounded time
// (file and stream I/O). The VM stages their arguments and result off
// the frame, parks the context and releases its global lock around the
// call; NativeFn itself is unchanged (the staging is transparent).
// Version 6: the array-payload side — getArrayView (a pinned view of a
// heap array record's cells: dims/strides/elemKind + the raw payload
// pointer, valid for the duration of the call; the GC is non-moving and
// the argument record is rooted for the call) and newArray (create a
// scalar-element array). The exception selector gained the
// NullPointer/OutOfBounds/DivideByZero families so native code raises
// the same catchable classes the language runtime does. Version 7:
// strides moved into the view itself (NativeArrayView.stridesInline —
// the v6 host-wide scratch buffer let a second live view overwrite the
// first's strides); ArgArray became an out-param because the view's
// strides pointer now points into itself. Version 8: the rank cap left
// the header (kMaxArrayRank moved to runtime/Config.h) and rose 8 -> 64,
// so stridesInline widened to match (the view is stack-allocated; dims
// still points into the record and grows nothing). Version 9: newArrayShaped
// (create an ND array from explicit dims) joined the table so matrix-family
// kernels can create [rows, cols] and beyond; newArray stays rank-1.
// Version 10: the array-view side — NativeArrayView gained `offset`
// (element units, matching strides; owning arrays report 0), getArrayView
// accepts view records (filling from the record's strides/offset instead
// of recomputing row-major ones; owning records keep the computed shape),
// and newArrayView creates a view over an existing array handle with the
// same validation the language's view creation applies. Version 11: the
// IO-parking and work-pool side — nowMs (monotonic clock, same domain as
// the park deadlines), parkForIo (register interest, unwind, re-enter on
// wake — the non-blocking-IO contract the net module builds on),
// moduleState (per-executor module globals with a teardown hook),
// wakeIoWaiters (close-side wake), currentContextId (the stable caller
// identity across park/re-entry) and submitWork (the host's generic
// native worker pool, joined at scheduler teardown). Version 12: the
// receiver's runtime instantiation — typeArgCount/typeArgKind/typeArgName
// let a native method read the type arguments its receiver was
// instantiated with (the runtime generic type-parameter information:
// count, per-argument RTK kind, per-argument canonical name).
// Version 13: the tensor device side (the v0.14.x device line) —
// setTensorDeviceHooks (the tensor module registers its device
// backend's allocator/mover/sync hooks at init; the VM routes device
// records through them and never links any vendor runtime itself),
// newArrayResident (mint a device-resident array record), and
// snapshotRngForDevice (an atomic "copy the MT19937 state + block
// position, then discard(n)" composite — single callback because two
// calls would race between concurrent carriers). NativeArrayView's
// reserved[0] now carries the array residency (0 = host payload, 1 =
// device payload; the fill sites set it).
//
// v14: the device hooks are fallible — every hook returns a
// TensorDeviceStatus (vendor-neutral error data; the VM converts a
// non-ok status into the language exception at the call site, the
// mapping table living once next to the record). alloc writes its
// pointer through an out parameter (a failed allocation has no
// pointer to return).
// v15: snapshotWindowRng — the multi-stream PRNG window draw (a private
// engine-state copy advanced by an offset, WITHOUT advancing the live engine).
// v16: the device id rides the payload boundary (the multi-GPU line):
// every TensorDeviceHooks entry except sync takes a leading int32_t device
// selector, newArrayResident takes a trailing device argument, and
// NativeArrayView's reserved[1] carries the device id of a device-
// resident payload (meaningful only when reserved[0] says device). One build still targets device 0; the
// plumbing lands now so the device choice never re-enters a signature.
#define NLANG_HOST_ABI_VERSION 16u

// Every NLang runtime value occupies one 8-byte frame cell (see
// kFrameSlotBytes in CompiledModule.h). Scalars of 4 bytes or less live
// in the LOW half of the cell (integers inline, floats as their bit
// pattern); the 8-byte family (long/double) uses all 8 bytes;
// strings/objects/lists carry a 32-bit heap handle in the low half.
#define NLANG_VALUE_SIZE 8

// parkForIo event bits (the module-side spelling; the VM's IoPoller.h
// mirrors these exact values for its poller layer).
#define NLANG_EV_READ 1u
#define NLANG_EV_WRITE 2u

// v13: the tensor device side. The tensor module registers its active
// device backend's hooks once at module init (the init sequence
// predates any user frame); the hooks are then immutable for the
// process lifetime. The VM routes device-resident array records through
// them (element transfers, creation zero-fill, GC release) and never
// links any vendor runtime itself.
//
//   alloc  - pooled device allocation (may not zero);
//   zero   - clear bytes to 0 (array creation's zero-init);
//   free   - return a block to the pool (GC release path);
//   copyH2D/copyD2H - byte-accurate host↔device transfer (the device
//            line's staging primitive; a "mirror" test backend
//            implements these as plain host memcpy);
//   sync   - full-device sync point (one op one sync discipline).
//
// All hooks are blocking (tensor natives run on the blocking channel,
// so the carrier lock is already released). Null in any slot = no
// device backend active (the creation and transfer paths raise the
// Base family by name).
//
// Every hook returns a TensorDeviceStatus. ok means the effect is
// observable (the blocking-hook invariant); anything else means the
// operation failed and the VM raises at the call site. The category
// to exception-family mapping lives once next to the record (see the
// DeviceStatusCategory note). free is the one exception to the
// raise-at-call-site rule: the GC release path cannot throw (it runs
// inside the sweep's invariants), so its status parks in the
// backend's sticky register and surfaces at the next observation
// point.

// The device operation's outcome, in vendor-neutral vocabulary — no
// vendor error code or vendor text enters this record (the vendor's
// wording rides the static detail string only). The VM's category →
// exception-family mapping (the single conversion table):
//   ok                -> no exception
//   out_of_memory     -> Base, detail text
//   bad_index         -> IndexOutOfBounds, the cell named
//   divide_by_zero    -> DivByZero, the cell named
//   fault             -> Base, detail text (the vendor runtime's
//                        wording; launch configuration, execution
//                        fault, context error)
typedef enum DeviceStatusCategory {
    DEVICE_OK = 0,
    DEVICE_OUT_OF_MEMORY,
    DEVICE_BAD_INDEX,
    DEVICE_DIVIDE_BY_ZERO,
    DEVICE_FAULT
} DeviceStatusCategory;

typedef struct TensorDeviceStatus {
    int32_t category;    // DeviceStatusCategory
    int32_t cell;        // the offending flat cell index (bad index /
                         // divide by zero); unused otherwise
    const char* detail;  // the vendor's static text (fault); null
                         // otherwise — a static string, never owned
} TensorDeviceStatus;

// ABI v16: every payload entry names the device its pointer lives on.
// Today the VM only ever passes 0; the vendor runtime's context
// selection belongs to the hook implementation (the VM stays vendor-
// neutral and never links one). sync follows the CURRENT context — the
// device selector would duplicate what the enqueue side already fixed.
struct TensorDeviceHooks {
    TensorDeviceStatus (*alloc)(int32_t device, void** out, size_t bytes);
    TensorDeviceStatus (*zero)(int32_t device, void* ptr, size_t bytes);
    TensorDeviceStatus (*free)(int32_t device, void* ptr);
    TensorDeviceStatus (*copyH2D)(int32_t device, void* dst,
                                  const void* src, size_t bytes);
    TensorDeviceStatus (*copyD2H)(int32_t device, void* dst,
                                  const void* src, size_t bytes);
    TensorDeviceStatus (*sync)();
};

struct NativeHost;
typedef struct NativeHost NativeHost;

// Native function signature.
//   host  - the host function table
//   ret   - points to one value slot that receives the return value
//   args  - base of the argument value slots (no `this` for namespace
//           functions; methods pass the receiver in slot 0)
//   argc  - declared parameter count (number of value slots in args)
typedef void (*NativeFn)(NativeHost* host, uint8_t* ret,
                         const uint8_t* args, int argc);

// Exception class selector for NativeHost.raiseException.
enum NativeExceptionKind {
    NEXC_Base = 0,        // base Exception (math argument/range errors, ...)
    NEXC_IOException = 1, // IOException (io/fs failures)
    //v6: same catchable families the language runtime raises, so native
    //kernels are catchable exactly like built-in operations.
    NEXC_NullPointer = 2, // NullPointerException (null array handle, ...)
    NEXC_OutOfBounds = 3, // IndexOutOfBoundsException (negative length, ...)
    NEXC_DivideByZero = 4 // DivByZeroException (integer kernel division)
};

// Scalar element kinds a native may create arrays of or inspect through a
// NativeArrayView. Values MIRROR the runtime RTK_* scalar codes in
// PrimitiveTypes.h (single source; keep in tie by this comment — same
// discipline as kMaxNativeStagedParams mirroring the compiler constant).
// The identity mapping lets the host map NEK -> RTK with no table.
// ABI v11 added the byte pair: io.toBytes creates byte arrays through
// newArray, and the narrow-cell convention (one int32 cell per element,
// low byte = value) is reported through NativeArrayView like any other
// kind.
enum NativeElemKind {
    NEK_Int32   = 0,   // RTK_Int32
    NEK_Float32 = 1,   // RTK_Float
    NEK_Byte    = 10,  // RTK_Byte
    NEK_UByte   = 11,  // RTK_UByte
    NEK_Int64   = 15,  // RTK_Long
    NEK_Float64 = 17   // RTK_Double
};

// A pinned view of one heap array record's payload, handed to a native
// through getArrayView. The record's cells back every pointer:
//   dims    - points INTO the record (the dims run; for rank 1 the
//             single dim equals the element count).
//   strides - points at stridesInline inside this view (row-major,
//             computed per call). Each view owns its storage: multiple
//             live views never share it. Copying the view struct leaves
//             strides pointing into the original — request a fresh view
//             instead of copying.
//   data    - the payload base (element 0). The GC is non-moving and the
//             argument record is rooted for the duration of the native
//             call, so the pointer is stable for the call — and only for
//             the call. Never cache it past return.
// elemCellBytes is the per-element stride in bytes (4, or 8 for the
// 8-byte scalar family); elemCount the total element count. offset is
// in ELEMENT units, the same unit as strides: element (i0, i1, ...)
// lives at data + (offset + sum(ik * strides[k])) * elemCellBytes.
// Owning arrays report 0 (data already points at element zero); a
// view record reports its recorded offset into the owning payload.
// ABI v13: reserved[0] carries the payload residency — 0 = host, 1 =
// device (data is a device pointer; only the device backend's kernels
// may dereference it, host code routes the operand through its slots).
// ABI v16: reserved[1] carries the device id of a device payload (read
// it only when reserved[0] is 1); host payloads keep it 0.
typedef struct NativeArrayView {
    int32_t rank;
    const int32_t* dims;
    const int32_t* strides;
    int64_t elemCount;
    uint8_t elemKind;      // NativeElemKind value
    uint8_t elemCellBytes;
    uint8_t reserved[2];   // v13/v16: [0] = residency, [1] = device id
    int64_t offset;        // ABI v10: element units, matches strides
    int32_t stridesInline[64];  // = kMaxArrayRank (asserted below); ABI v8: widened with the rank cap
    void* data;
} NativeArrayView;

struct NativeHost {
    uint32_t abiVersion;

    // Read a string argument by heap handle. The returned pointer is
    // valid only until the next host callback; copy it immediately.
    // Handle 0 is the live empty string (ruling D26): getString(0)
    // returns a valid pointer to "", never NULL.
    const char* (*getString)(NativeHost* self, int32_t handle);

    // Create a heap string from UTF-8; returns its handle. Never
    // returns 0 — empty content (and a null pointer) creates a fresh
    // non-interned empty object (handle >= 1). A declared-string
    // native must produce its result through this call: writing 0
    // into the result cell is a contract violation the host reports
    // as "returned null string" (ruling D27).
    int32_t (*newString)(NativeHost* self, const char* utf8);

    // Create a List<string> in one call; returns its heap handle. Used by
    // fs.listFiles so native code never needs the VM's list layout.
    int32_t (*newListString)(NativeHost* self, const char* const* items, int count);

    // Write text verbatim to the program's standard output.
    void (*writeOutput)(NativeHost* self, const char* text);

    // Write text verbatim to the program's diagnostic channel: stderr in
    // console mode, the session's single merged output view when a host
    // owns the streams (io.eprint's route).
    void (*writeError)(NativeHost* self, const char* text);

    // Read one line from standard input (trailing CR/LF stripped).
    // Returns null at end of input — an empty input line returns "".
    // The returned pointer is valid only until the next host callback.
    const char* (*readLine)(NativeHost* self);

    // v4 input side. readToken skips whitespace (across lines) and
    // returns the next token, or null at end of input — the host raises
    // before returning null, so natives treat null as defensive only.
    // readChar decodes one full UTF-8 scalar; 0 = ok — at end of input
    // or on invalid UTF-8 the host raises before returning, so natives
    // never observe the 1. hasInput never raises: 1 = a readLine would
    // still produce a line.
    const char* (*readToken)(NativeHost* self);
    int (*readChar)(NativeHost* self, uint32_t* outChar);
    int (*hasInput)(NativeHost* self);

    // Raise an NLang exception and never return.
    void (*raiseException)(NativeHost* self, int exceptionKind, const char* message);

    // PRNG access (state owned by the VM).
    uint32_t (*nextRandom)(NativeHost* self);
    void (*seedRandom)(NativeHost* self, int32_t seed);

    // v6 array side. getArrayView fills *out with a pinned view of the
    // array record behind the heap handle. A null handle raises
    // NullPointerException, a non-array record raises the base Exception;
    // on return the view is fully populated (returns 1 — errors raise,
    // there is no non-raising outcome). The view is valid only for the
    // duration of this native call.
    int (*getArrayView)(NativeHost* self, int32_t handle, NativeArrayView* out);

    // Create a new array of a scalar element kind with `length` elements,
    // zero-initialized; returns its heap handle. A negative length raises
    // the OutOfBounds family (language-side `T[-n]` alignment), a length
    // beyond the allocation guard raises the same family. The handle is
    // rooted until the native writes it into its return cell.
    int32_t (*newArray)(NativeHost* self, uint8_t elemKind, int32_t length);

    // Create a new ND array of a scalar element kind with the given rank and
    // dims, zero-initialized; returns its heap handle. A rank outside
    // [1, kMaxArrayRank] raises by name; negative dims and the allocation
    // cap raise in the shared sized-creation choke point (OutOfBounds
    // family). The handle is rooted until the native writes it into its
    // return cell. (v9)
    int32_t (*newArrayShaped)(NativeHost* self, uint8_t elemKind, int32_t rank, const int32_t* dims);

    // Create a view over the array (or view) behind targetHandle: the given
    // rank/dims/strides/offset (offset in ELEMENT units) with the same
    // validation the language's view creation applies — null or non-array
    // targets, rank out of range, element-type mismatch, bad dims and
    // range-exceeds-bounds all raise (the OutOfBounds family). Returns the
    // view's heap handle; the handle is rooted until the native writes it
    // into its return cell. (v10)
    int32_t (*newArrayView)(NativeHost* self, int32_t targetHandle, int32_t rank, const int32_t* dims, const int32_t* strides, int64_t offset);

    //v11: monotonic milliseconds, same clock domain as the VM IO poller
    //deadlines (steady_clock epoch; not wall time).
    long long (*nowMs)(NativeHost* self);
    //v11: register fd interest + optional absolute deadline (steady-clock
    //ms; hasDeadline=0 means none), then unwind the calling native.
    //Returns 0 on the first entry (interest registered; the native MUST
    //return immediately — the VM parks the context and releases the
    //carrier) and 1 on re-entry after wake (the VM re-executes the
    //CallNative with the same staged arguments; retry the operation).
    //fd < 0 = pure deadline park. Only callable from a blocking-channel
    //native (the GIL is released on entry; the host re-takes it).
    //Re-entry re-executes the CallNative instruction (the dispatch loop
    //pins pc at it), so parking exists only for instruction-level
    //(script-dispatched) native calls; host-initiated native calls —
    //the native-main root window, the host bridge's native-formal arm
    //— carry no such re-execution anchor and must not park (external
    //review M13).
    //Note: the armed flag is set by ANY WakeForIo wake, including
    //interpreter-path parks (e.g. thread.sleep) that never called
    //parkForIo; a stale armed makes the next blocking-native's first
    //parkForIo return 1 (one extra retry). Harmless by contract — the
    //caller re-checks its condition on every return — do not "fix" it
    //by clearing armed on other wake paths.
    int (*parkForIo)(NativeHost* self, int fd, unsigned events, long long deadlineMs, int hasDeadline);
    //v11: per-executor host-managed module state slot keyed by tag. The
    //host calls ctor() once per executor, stores the pointer with dtor,
    //and runs dtor at executor teardown (before the module unloads —
    //modules never unload). Later calls return the stored pointer.
    void* (*moduleState)(NativeHost* self, const char* tag, void* (*ctor)(void), void (*dtor)(void*));
    //v11: unregister poller interest for fd and wake every context parked
    //on it (socket close path — the module cannot reach the VM poller
    //directly). Callable without the GIL; the host takes it internally.
    void (*wakeIoWaiters)(NativeHost* self, int fd);
    //v11: id of the context executing the current blocking-native call
    //(stable across park/re-entry — the net module's per-direction waiter
    //guard needs it to tell its own re-entry from a foreign concurrent
    //claim, plan ruling R14). Only callable from a blocking-channel
    //native (the staged calling context).
    int (*currentContextId)(NativeHost* self);
    //v11: run fn(arg) on the host's generic native worker pool (workers
    //hold no GIL); on completion the pool sends one byte to ackFd (a
    //socket fd owned by the caller; -1 = no ack). priority: 0 normal,
    //1 high (two-level queue — a slow getaddrinfo never starves short
    //jobs; libuv uv_queue_work precedent). The pool starts lazily and
    //is joined at scheduler teardown (after all carriers stop, the R8
    //precondition). Callable from any channel; returns 0.
    int (*submitWork)(NativeHost* self, void (*fn)(void*), void* arg, int ackFd, int priority);
    //v12: the current call's receiver type arguments (the runtime
    //generic type-parameter information). All three read the
    //instantiation descriptor of the object the native method call in
    //flight received as its receiver (slot 0). typeArgCount answers the
    //type-argument count (0 for a non-generic or absent receiver); i
    //past the count answers the out-of-range sentinels — typeArgKind
    //0xFF (the invalid-RTK sentinel) and typeArgName NULL. The name
    //pointer stays valid until the native returns (names are interned
    //for the process lifetime).
    uint32_t (*typeArgCount)(NativeHost* self);
    uint8_t (*typeArgKind)(NativeHost* self, uint32_t i);
    const char* (*typeArgName)(NativeHost* self, uint32_t i);

    void (*setTensorDeviceHooks)(NativeHost* self, const TensorDeviceHooks* hooks);

    //v13: mint a device-resident array record of a scalar element kind
    //with the given rank and dims, zero-initialized through the hooks;
    //returns its heap handle (rooted until the native writes it into
    //its return cell, same contract as newArrayShaped). byte/ubyte
    //kinds raise the Base family (I/O buffers stay host-resident —
    //io/net read and write those payloads directly). No hooks active
    //raises the Base family (backend=cpu calling toDevice's path).
    //v16: device selects which device's payload the record lands on
    //(0 today; the VM pins the heap record's device entry to it).
    int32_t (*newArrayResident)(NativeHost* self, uint8_t elemKind, int32_t rank, const int32_t* dims, int32_t device);

    //v13: ATOMIC "snapshot the PRNG state, then discard n" — copies the
    //engine's 624-word state into out624 and its block position into
    //outPosition, then advances the engine by n outputs, all inside the
    //engine mutex. One callback, not a read/discard pair: two calls
    //would race between concurrent carriers and hand two uniform draws
    //the same snapshot (duplicate sequences). n may be 0 (snapshot only).
    void (*snapshotRngForDevice)(NativeHost* self, uint32_t* out624, uint32_t* outPosition, int64_t n);

    //v15: draw n words at (position + offset) WITHOUT advancing the live
    //engine — the multi-stream PRNG window; a private state copy keeps the
    //main stream coherent, and host-thread enqueue order fixes the offset.
    void (*snapshotWindowRng)(NativeHost* self, uint32_t* outWords, int64_t n, int64_t offset);
};

// Version probe. The host resolves this export first and rejects the
// module when it is missing or reports a different NLANG_HOST_ABI_VERSION
// — an incompatible module's init must never run, or its registrations
// (pointers into a library the host immediately unloads) would leak.
#define NLANG_NATIVE_ABI_SYMBOL nlang_abi_version
typedef unsigned (*NativeAbiProbeFn)(void);

// Module entry point. The host loads the shared library, validates
// `nlang_abi_version`, then resolves `nlang_native_init` and calls it
// once. The module registers each of its functions via `reg`
// (non-blocking: runs while the VM's global lock is held) or
// `regBlocking` (may take unbounded time: the VM stages args/result,
// parks the context and releases the lock around the call), forwarding
// the opaque `registry` pointer. Registering the same ns.name through
// both channels is a load error.
// The init return value is informational (the probe is the single
// version authority); returning NLANG_HOST_ABI_VERSION keeps hand-written
// modules honest.
typedef void (*RegisterNativeFn)(void* registry, const char* ns,
                                 const char* name, NativeFn fn);
typedef void (*RegisterNativeBlockingFn)(void* registry, const char* ns,
                                         const char* name, NativeFn fn);
// v13: the init signature gains the NativeHost* — the tensor module
// registers its device backend's hooks through setTensorDeviceHooks at
// init (per-executor: each executor's loader runs the module init once,
// so the hooks land per executor with that executor's host; see the
// NativeSetTensorDeviceHooks contract for the immutability rule).
typedef int (*NativeModuleInitFn)(void* registry, RegisterNativeFn reg,
                                  RegisterNativeBlockingFn regBlocking,
                                  NativeHost* host);

#define NLANG_NATIVE_INIT_SYMBOL nlang_native_init

// Export helper for native modules. The entry point must use C linkage so
// the host can resolve it by its unmangled name on every platform.
#ifdef __cplusplus
#  define NLANG_NATIVE_EXTERN extern "C"
#else
#  define NLANG_NATIVE_EXTERN
#endif
#if defined(_WIN32)
#  define NLANG_NATIVE_EXPORT NLANG_NATIVE_EXTERN __declspec(dllexport)
#else
#  define NLANG_NATIVE_EXPORT \
    NLANG_NATIVE_EXTERN __attribute__((visibility("default")))
#endif

// Convenience macro for the module's exports. It emits a correct
// nlang_abi_version probe automatically, so a module only writes its
// init body:
//   NLANG_DEFINE_NATIVE_INIT {
//       reg(registry, "math", "sqrt", &...);          //fast, GIL held
//       regBlocking(registry, "io", "readFile", &...); //may wait
//   }
#define NLANG_DEFINE_NATIVE_INIT \
    NLANG_NATIVE_EXPORT unsigned nlang_abi_version(void) { \
        return NLANG_HOST_ABI_VERSION; \
    } \
    NLANG_NATIVE_EXPORT int nlang_native_init(void* registry, \
                                              RegisterNativeFn reg, \
                                              RegisterNativeBlockingFn regBlocking, \
                                              NativeHost* host)

#ifdef __cplusplus
} // extern "C"
#endif

// ---------------------------------------------------------------------------
// C++ convenience wrappers (header-only) live in NativeHostHelpers.h —
// split from this header at the source-size guard so the C ABI stays the
// single stable surface. Included here so existing consumers keep the
// wrappers transitively.
// ---------------------------------------------------------------------------

#include <nlang/vm/NativeHostHelpers.h>
