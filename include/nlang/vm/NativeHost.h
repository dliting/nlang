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
// mismatch.
#define NLANG_HOST_ABI_VERSION 1u

// Every NLang runtime value occupies one 32-bit slot. Integers and the
// bit-pattern of floats are stored inline; strings/objects/lists carry a
// 32-bit heap handle.
#define NLANG_VALUE_SIZE 4

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
    NEXC_IOException = 1  // IOException (io/fs failures)
};

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

    // Read one line from standard input (trailing CR/LF stripped). The
    // returned pointer is valid only until the next host callback.
    const char* (*readLine)(NativeHost* self);

    // Raise an NLang exception and never return.
    void (*raiseException)(NativeHost* self, int exceptionKind,
                           const char* message);

    // PRNG access (state owned by the VM).
    uint32_t (*nextRandom)(NativeHost* self);
    void (*seedRandom)(NativeHost* self, int32_t seed);
};

// Module entry point. The host loads the shared library, resolves
// `nlang_native_init`, and calls it once. The module registers each of
// its functions via `reg`, forwarding the opaque `registry` pointer.
// Returns NLANG_HOST_ABI_VERSION so the host can verify compatibility.
typedef void (*RegisterNativeFn)(void* registry, const char* ns,
                                 const char* name, NativeFn fn);
typedef int (*NativeModuleInitFn)(void* registry, RegisterNativeFn reg);

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
//   NLANG_DEFINE_NATIVE_INIT { reg(registry, "math", "sqrt", &...); ... }
#define NLANG_DEFINE_NATIVE_INIT \
    NLANG_NATIVE_EXPORT int nlang_native_init(void* registry, \
                                              RegisterNativeFn reg)

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
    std::memcpy(&value, args + slot * NLANG_VALUE_SIZE,
                NLANG_VALUE_SIZE);
    return value;
}

inline float ArgFloat(const uint8_t* args, int slot) {
    int32_t bits = ArgInt(args, slot);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(float));
    return value;
}

inline std::string ArgString(NativeHost* host, const uint8_t* args,
                             int slot) {
    int32_t handle = ArgInt(args, slot);
    const char* text = host->getString(host, handle);
    return text ? std::string(text) : std::string();
}

inline void ReturnInt(uint8_t* ret, int32_t value) {
    std::memcpy(ret, &value, NLANG_VALUE_SIZE);
}

inline void ReturnFloat(uint8_t* ret, float value) {
    int32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(float));
    std::memcpy(ret, &bits, NLANG_VALUE_SIZE);
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
