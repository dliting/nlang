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
};

//The plugin's exported symbol set (the spec D5 three symbols).
#define NLANG_DEVICE_ABI_SYMBOL nlang_device_abi_version
#define NLANG_DEVICE_NAME_SYMBOL nlang_device_backend_name
#define NLANG_DEVICE_INIT_SYMBOL nlang_device_backend_init

} // namespace tensor
