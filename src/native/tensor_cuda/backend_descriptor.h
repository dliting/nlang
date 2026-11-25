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

struct DeviceBackendDescriptor {
    const void* ops;            //the TensorOps table (registry casts)
    const void* hooks;          //the TensorDeviceHooks (ABI v13's struct)
    const char* name;           //"cuda" / "hip" / ...
    const char* description;    //the human-readable device line
    int deviceCount;            //the probe result (0 = no device)
    //--- v0.14.9: the multi-stream orchestration (the pool + the
    //current-stream stack live in the plugin; the registry's stream
    //natives dispatch through these). Null = the backend has no
    //stream pool (the registry's stream natives become no-ops).
    int (*streamCreate)();      //a pool handle (>=0), -1 = exhausted
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
