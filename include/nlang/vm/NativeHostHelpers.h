#pragma once

/*---
    NativeHostHelpers.h — C++ convenience wrappers over the native-host
    C ABI (argument readers, return writers, minting shorthands).
    Header-only; split from NativeHost.h at the source-size guard so the
    C ABI header stays the single stable surface. NativeHost.h includes
    this header, so existing consumers keep the wrappers transitively.
---*/

#include <nlang/vm/NativeHost.h>

// ---------------------------------------------------------------------------
// C++ convenience wrappers (header-only). These keep native function bodies
// terse while never exposing VM internals.
// ---------------------------------------------------------------------------
#ifdef __cplusplus

#include <cstring>
#include <string>

#include <nlang/runtime/Config.h>

//The core struct stays C-compatible (plain [64]); this pins the coupling.
static_assert(nlang::kMaxArrayRank == 64,
    "NativeArrayView.stridesInline must track kMaxArrayRank "
    "(bump NLANG_HOST_ABI_VERSION when it changes)");

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

// v7: array argument as a pinned payload view. Raises (never returns)
// when the handle is null or not an array record — the same behavior the
// language gives null/non-array receivers. Out-param form: the view's
// strides point into itself, so the struct cannot travel by value.
inline void ArgArray(NativeHost* host, const uint8_t* args, int slot,
                     NativeArrayView* out) {
    out->rank = 0;
    out->dims = nullptr;
    out->strides = nullptr;
    out->elemCount = 0;
    out->elemKind = 0;
    out->elemCellBytes = 0;
    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->offset = 0;
    out->data = nullptr;
    host->getArrayView(host, ArgInt(args, slot), out);
}

// v6: mint a zero-initialized array; returns its heap handle.
inline int32_t NewArray(NativeHost* host, uint8_t elemKind, int32_t length) {
    return host->newArray(host, elemKind, length);
}

// v9: mint a zero-initialized ND array from explicit dims.
inline int32_t NewArrayShaped(NativeHost* host, uint8_t elemKind,
                              int32_t rank, const int32_t* dims) {
    return host->newArrayShaped(host, elemKind, rank, dims);
}

// v10: mint a view over an existing array handle (validation and units
// as the newArrayView slot documents).
inline int32_t NewArrayView(NativeHost* host, int32_t targetHandle,
                            int32_t rank, const int32_t* dims,
                            const int32_t* strides, int64_t offset) {
    return host->newArrayView(host, targetHandle, rank, dims, strides,
                              offset);
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
