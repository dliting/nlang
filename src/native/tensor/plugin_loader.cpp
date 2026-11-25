/*---
    plugin_loader.cpp - cross-platform native shared-library loading for
    the CUDA device backend plugin (design 2026-11-15-p3-routing-design).
    See plugin_loader.h for the contract.
---*/
#include "plugin_loader.h"
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace tensor {

std::string OwnModuleDirectory() {
#if defined(_WIN32)
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&OwnModuleDirectory, &self);
    char selfPath[MAX_PATH] = {};
    GetModuleFileNameA(self, selfPath, MAX_PATH);
    std::string path = selfPath;
#else
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(&OwnModuleDirectory), &info) == 0
        || info.dli_fname == nullptr)
        return std::string();
    std::string path = info.dli_fname;
#endif
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string()
         : path.substr(0, slash + 1);
}

#if defined(_WIN32)
void* OpenSharedPlugin(const std::string& path) {
    return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
}
void* ResolvePluginSymbol(void* handle, const char* name) {
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), name));
}
void CloseSharedPlugin(void* handle) {
    FreeLibrary(static_cast<HMODULE>(handle));
}
#else
void* OpenSharedPlugin(const std::string& path) {
    //RTLD_LOCAL keeps the plugin's CUDA runtime symbols from interposing
    //any other object in the process.
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}
void* ResolvePluginSymbol(void* handle, const char* name) {
    return dlsym(handle, name);
}
void CloseSharedPlugin(void* handle) {
    dlclose(handle);
}
#endif

}   //namespace tensor
