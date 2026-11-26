/*---
    backend_descriptor.h — the DeviceBackend plugin contract's wire
    format (spec D5): the descriptor block the plugin's init returns
    and the registry (nlang_tensor.dll) reads. Both sides include this
    header; the layout is the contract.
---*/
#pragma once

#include <nlang/vm/NativeHost.h>

namespace tensor {

struct TensorOps;   //TensorOps.h (the registry side includes it)

//The device-ABI contract version, owned once by this header (both
//sides include it; the probe compares against it). v2: streamCreate
//takes the device selector (the multi-GPU line, M-G1). The plugin's
//nlang_device_abi_version export must return this exact value — a
//mismatch means the descriptor layout or the stream contract moved.
inline constexpr unsigned NLANG_DEVICE_ABI_VERSION = 2u;

//The pure side of the load-time probe: true = the plugin's reported
//version is exactly the one this registry speaks. Kept header-inline
//so a unit test covers the reject path without a stub plugin.
inline bool DeviceAbiMatches(unsigned reported) {
    return reported == NLANG_DEVICE_ABI_VERSION;
}

//--- stream-handle encoding (the registry and the plugin's shared
//contract). A stream handle is `device * kNativeMaxStreams + slot`:
//(device 0, slot 0) — handle 0 — is the implicit main stream, the
//sentinel every device-invisible path uses; slot 0 is reserved per
//device, so the first pooled handle is 1. The plugin sizes its
//per-device stream pools by kNativeMaxStreams (ABI-visible: changing
//it is a device-ABI bump); the Push nesting cap bounds the
//registry-side and the plugin-side push stacks with one number so
//they can never disagree.
inline constexpr int kNativeMaxStreams = 16;
inline constexpr int kNativeMaxStreamPushDepth = 64;

//The device census ceiling, shared the same way: the plugin clamps
//its per-device state tables and its exported deviceCount to it, so
//the user-facing census can never name a device the plugin cannot
//serve. (A bound only one side knows drifts — the same reason the
//pool width and the push cap are shared above.)
inline constexpr int kNativeMaxDevices = 16;

//Handles are non-negative by construction (Encode's domain). The
//decoders are total over every int — C++ truncating division makes
//the negative image defined but meaningless (Decode(-1) is (0, -1))
//— so the contract sits with the consumers: handle <= 0 is the
//sentinel/no-pool case, checked before any decode.
inline int EncodeStreamHandle(int device, int slot) {
    return device * kNativeMaxStreams + slot;
}
inline int DecodeStreamDevice(int handle) {
    return handle / kNativeMaxStreams;
}
inline int DecodeStreamSlot(int handle) {
    return handle % kNativeMaxStreams;
}

struct DeviceBackendDescriptor {
    const void* ops;            //the TensorOps table (registry casts)
    const void* hooks;          //the TensorDeviceHooks (ABI v16's struct)
    const char* name;           //"cuda" / "hip" / ...
    const char* description;    //the human-readable device line
    int deviceCount;            //the probe result (0 = no device)
    //--- v0.14.9: the multi-stream orchestration (the pool + the
    //current-stream stack live in the plugin; the registry's stream
    //natives dispatch through these). Null = the backend has no
    //stream pool (the registry's stream natives become no-ops).
    //--- device ABI v2: streamCreate names the device the pool slot
    //is created on; the returned handle encodes that device (§ above).
    int (*streamCreate)(int device);  //a pool handle, -1 = none usable
    void (*streamDestroy)(int handle);
    void (*streamPush)(int handle);   //the ops after this launch there
    void (*streamPop)();
    void (*streamJoin)(int handle);   //the current stream waits for it
};

//The plugin's exported symbol set (the spec D5 three symbols).
#define NLANG_DEVICE_ABI_SYMBOL nlang_device_abi_version
#define NLANG_DEVICE_NAME_SYMBOL nlang_device_backend_name
#define NLANG_DEVICE_INIT_SYMBOL nlang_device_backend_init

//Export decorator for the three C symbols: dllexport on Windows; default
//visibility elsewhere (matches the NLANG_NATIVE_EXPORT pattern in
//NativeHost.h; the symbols are already C-linked via the extern "C" block).
#if defined(_WIN32)
#  define NLANG_DEVICE_EXPORT __declspec(dllexport)
#else
#  define NLANG_DEVICE_EXPORT __attribute__((visibility("default")))
#endif

} // namespace tensor
