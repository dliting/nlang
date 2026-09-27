#pragma once

// NativeLibraryLoader — discovers and loads native modules
// (`nlang_<ns>.dll` / `libnlang_<ns>.so`) on demand and invokes their
// `nlang_native_init` entry point.
//
// Both the standard library and third-party libraries are loaded through
// this one class. The loader is deliberately decoupled from VmExecutor:
// when a module initializes, each registration is forwarded through a
// caller-supplied std::function callback, so this class needs no knowledge
// of the VM's internal native table.

#include "nlang/vm/NativeHost.h"

#include <functional>
#include <string>
#include <vector>

namespace nlang {

// Called once per function a module registers during init.
using RegisterNativeCallback =
    std::function<void(const std::string& ns, const std::string& name,
                       NativeFn fn)>;

class NativeLibraryLoader {
public:
    NativeLibraryLoader();
    ~NativeLibraryLoader();

    NativeLibraryLoader(const NativeLibraryLoader&) = delete;
    NativeLibraryLoader& operator=(const NativeLibraryLoader&) = delete;

    // Append a directory searched for native modules.
    void AddSearchDir(std::string dir);

    // Ensure the module implementing namespace `ns` is loaded. The first
    // call loads the shared library and runs its init; later calls are
    // no-ops. `registerFn` receives every function the module exports.
    // Throws std::runtime_error (with a message naming the searched paths)
    // when the module cannot be found, has no nlang_native_init entry, or
    // reports an incompatible ABI version.
    void EnsureLoaded(const std::string& ns,
                      RegisterNativeCallback registerFn);

    // Directory containing the running executable (default search location).
    static std::string ExecutableDir();

private:
    struct LoadedModule {
        std::string ns;
        void* handle = nullptr;
    };

    bool IsLoaded(const std::string& ns) const;
    // Locate the module file across search dirs; empty if not found.
    std::string LocateModuleFile(const std::string& ns) const;

    std::vector<std::string> m_searchDirs;
    std::vector<LoadedModule> m_loaded;
};

} // namespace nlang
