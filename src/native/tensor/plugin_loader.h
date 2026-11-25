/*---
    plugin_loader.h - cross-platform native shared-library loading for
    the CUDA device backend plugin (dlopen/dlsym/dlclose on POSIX,
    LoadLibrary/GetProcAddress/FreeLibrary on Windows). Split out of
    backend_registry.cpp so that registry stays under the source-size
    guard's file limit; see plugin_loader.cpp for the contract.
---*/
#pragma once

#include <string>

namespace tensor {

//The native dir of this module, resolved from a function address inside
//it (no search-path games). Windows: GetModuleHandleEx FROM_ADDRESS;
//POSIX: dladdr maps the address to its containing object (.so). Taking
//the address of this function pins its code location in the module even
//under inlining.
std::string OwnModuleDirectory();

//Platform-split shared-library loading.
void* OpenSharedPlugin(const std::string& path);
void* ResolvePluginSymbol(void* handle, const char* name);
void CloseSharedPlugin(void* handle);

//The plugin file name sits next to this module in the native dir.
#if defined(_WIN32)
constexpr const char* kCudaPluginName = "nlang_tensor_cuda.dll";
#else
constexpr const char* kCudaPluginName = "libnlang_tensor_cuda.so";
#endif

}   //namespace tensor
