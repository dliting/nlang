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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bump on any incompatible change to NativeHost, NativeFn or the value
// layout. A module's nlang_native_init returns this; the host rejects a
// mismatch. Version 2: the value slot widened from 4 to 8 bytes (the
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
// the argument record is rooted for the call) and newArray (mint a
// scalar-element array). The exception selector gained the
// NullPointer/OutOfBounds/DivideByZero families so native code raises
// the same catchable classes the language runtime does.
#define NLANG_HOST_ABI_VERSION 6u

// Every NLang runtime value occupies one 8-byte frame cell (see
// kFrameSlotBytes in CompiledModule.h). Scalars of 4 bytes or less live
// in the LOW half of the cell (integers inline, floats as their bit
// pattern); the 8-byte family (long/double) uses all 8 bytes;
// strings/objects/lists carry a 32-bit heap handle in the low half.
#define NLANG_VALUE_SIZE 8

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

// Scalar element kinds a native may mint arrays of or inspect through a
// NativeArrayView. Values MIRROR the runtime RTK_* scalar codes in
// PrimitiveTypes.h (single source; keep in tie by this comment — same
// discipline as kMaxNativeStagedParams mirroring the compiler constant).
// The identity mapping lets the host map NEK -> RTK with no table.
enum NativeElemKind {
    NEK_Int32   = 0,   // RTK_Int32
    NEK_Float32 = 1,   // RTK_Float
    NEK_Int64   = 15,  // RTK_Long
    NEK_Float64 = 17   // RTK_Double
};

// A pinned view of one heap array record's payload, handed to a native
// through getArrayView. The record's cells back every pointer:
//   dims    - points INTO the record (the dims run; for rank 1 the
//             single dim equals the element count).
//   strides - points at host-owned storage (row-major, computed per
//             call; read-only, thread-safe).
//   data    - the payload base (element 0). The GC is non-moving and the
//             argument record is rooted for the duration of the native
//             call, so the pointer is stable for the call — and only for
//             the call. Never cache it past return.
// elemCellBytes is the per-element stride in bytes (4, or 8 for the
// 8-byte scalar family); elemCount the total element count.
typedef struct NativeArrayView {
    int32_t rank;
    const int32_t* dims;
    const int32_t* strides;
    int64_t elemCount;
    uint8_t elemKind;      // NativeElemKind value
    uint8_t elemCellBytes;
    uint8_t reserved[2];
    void* data;
} NativeArrayView;

struct NativeHost {
    uint32_t abiVersion;

    // Read a string argument by heap handle. The returned pointer is
    // valid only until the next host callback; copy it immediately.
    const char* (*getString)(NativeHost* self, int32_t handle);

    // Create a heap string from UTF-8; returns its handle.
    int32_t (*newString)(NativeHost* self, const char* utf8);

    // Create a List<string> in one call; returns its heap handle. Used by
    // fs.listFiles so native code never needs the VM's list layout.
    int32_t (*newListString)(NativeHost* self,
                             const char* const* items, int count);

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
    // readChar decodes one full UTF-8 scalar; 0 = ok, 1 = end of input
    // (host raised). hasInput never raises: 1 = a readLine would still
    // produce a line.
    const char* (*readToken)(NativeHost* self);
    int (*readChar)(NativeHost* self, uint32_t* outChar);
    int (*hasInput)(NativeHost* self);

    // Raise an NLang exception and never return.
    void (*raiseException)(NativeHost* self, int exceptionKind,
                           const char* message);

    // PRNG access (state owned by the VM).
    uint32_t (*nextRandom)(NativeHost* self);
    void (*seedRandom)(NativeHost* self, int32_t seed);

    // v6 array side. getArrayView fills *out with a pinned view of the
    // array record behind the heap handle. A null handle raises
    // NullPointerException, a non-array record raises the base Exception;
    // on return the view is fully populated (returns 1 — errors raise,
    // there is no non-raising outcome). The view is valid only for the
    // duration of this native call.
    int (*getArrayView)(NativeHost* self, int32_t handle,
                        NativeArrayView* out);

    // Mint a new array of a scalar element kind with `length` elements,
    // zero-initialized; returns its heap handle. A negative length raises
    // the OutOfBounds family (language-side `T[-n]` alignment), a length
    // beyond the allocation guard raises the same family. The handle is
    // rooted until the native writes it into its return cell.
    int32_t (*newArray)(NativeHost* self, uint8_t elemKind, int32_t length);
};

// Module entry point. The host loads the shared library, resolves
// `nlang_native_init`, and calls it once. The module registers each of
// its functions via `reg` (non-blocking: runs while the VM's global
// lock is held) or `regBlocking` (may take unbounded time: the VM
// stages args/result, parks the context and releases the lock around
// the call), forwarding the opaque `registry` pointer. Registering the
// same ns.name through both channels is a load error.
// Returns NLANG_HOST_ABI_VERSION so the host can verify compatibility.
typedef void (*RegisterNativeFn)(void* registry, const char* ns,
                                 const char* name, NativeFn fn);
typedef void (*RegisterNativeBlockingFn)(void* registry, const char* ns,
                                         const char* name, NativeFn fn);
typedef int (*NativeModuleInitFn)(void* registry, RegisterNativeFn reg,
                                  RegisterNativeBlockingFn regBlocking);

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

// Convenience macro for the entry point:
//   NLANG_DEFINE_NATIVE_INIT {
//       reg(registry, "math", "sqrt", &...);          //fast, GIL held
//       regBlocking(registry, "io", "readFile", &...); //may wait
//   }
#define NLANG_DEFINE_NATIVE_INIT \
    NLANG_NATIVE_EXPORT int nlang_native_init(void* registry, \
                                              RegisterNativeFn reg, \
                                              RegisterNativeBlockingFn regBlocking)

#ifdef __cplusplus
} // extern "C"
#endif

// ---------------------------------------------------------------------------
// C++ convenience wrappers (header-only). These keep native function bodies
// terse while never exposing VM internals.
// ---------------------------------------------------------------------------
#ifdef __cplusplus

#include <cstring>
#include <string>

namespace nlang {
namespace native {

inline int32_t ArgInt(const uint8_t* args, int slot) {
    int32_t value = 0;
    //4-byte kinds live in the low half of the 8-byte cell.
    std::memcpy(&value, args + slot * NLANG_VALUE_SIZE, sizeof(value));
    return value;
}

inline float ArgFloat(const uint8_t* args, int slot) {
    int32_t bits = ArgInt(args, slot);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(float));
    return value;
}

inline double ArgDouble(const uint8_t* args, int slot) {
    double value = 0.0;
    //The 8-byte kinds fill the whole cell.
    std::memcpy(&value, args + slot * NLANG_VALUE_SIZE, sizeof(value));
    return value;
}

inline int64_t ArgLong(const uint8_t* args, int slot) {
    int64_t value = 0;
    std::memcpy(&value, args + slot * NLANG_VALUE_SIZE, sizeof(value));
    return value;
}

// v6: array argument as a pinned payload view. Raises (never returns)
// when the handle is null or not an array record — the same behavior the
// language gives null/non-array receivers.
inline NativeArrayView ArgArray(NativeHost* host, const uint8_t* args,
                                int slot) {
    NativeArrayView view;
    view.rank = 0;
    view.dims = nullptr;
    view.strides = nullptr;
    view.elemCount = 0;
    view.elemKind = 0;
    view.elemCellBytes = 0;
    view.reserved[0] = 0;
    view.reserved[1] = 0;
    view.data = nullptr;
    host->getArrayView(host, ArgInt(args, slot), &view);
    return view;
}

// v6: mint a zero-initialized array; returns its heap handle.
inline int32_t NewArray(NativeHost* host, uint8_t elemKind, int32_t length) {
    return host->newArray(host, elemKind, length);
}

inline std::string ArgString(NativeHost* host, const uint8_t* args,
                             int slot) {
    int32_t handle = ArgInt(args, slot);
    const char* text = host->getString(host, handle);
    return text ? std::string(text) : std::string();
}

inline void ReturnInt(uint8_t* ret, int32_t value) {
    //Low-half write: no kind-correct consumer reads the upper half of
    //the return cell.
    std::memcpy(ret, &value, sizeof(value));
}

inline void ReturnFloat(uint8_t* ret, float value) {
    int32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(float));
    std::memcpy(ret, &bits, sizeof(bits));
}

inline void ReturnDouble(uint8_t* ret, double value) {
    std::memcpy(ret, &value, sizeof(value));
}

inline void ReturnLong(uint8_t* ret, int64_t value) {
    std::memcpy(ret, &value, sizeof(value));
}

inline void ReturnString(NativeHost* host, uint8_t* ret,
                         const std::string& value) {
    int32_t handle = host->newString(host, value.c_str());
    ReturnInt(ret, handle);
}

// Raise and return a dummy value so a function body can write
// `return Raise(...)` after the noreturn call (defensive for compilers
// that do not treat the callback as noreturn).
inline void Raise(NativeHost* host, int exceptionKind,
                  const std::string& message) {
    host->raiseException(host, exceptionKind, message.c_str());
}

} // namespace native
} // namespace nlang

#endif // __cplusplus
