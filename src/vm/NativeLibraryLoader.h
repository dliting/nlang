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
//
// Thread model: instances are not internally synchronized — calls must be
// serialized by the caller (the VM loads modules under its global lock).
// The fallback directory list is computed lazily on first use, which is
// safe only under that serialization.

#include "nlang/vm/NativeHost.h"

#include <functional>
#include <string>
#include <vector>

namespace nlang {

// Called once per function a module registers during init. The
// blocking flag carries the ABI v5 channel the module chose: a
// blocking native may take unbounded time, so the VM stages its
// arguments/result and releases the global lock around the call.
using RegisterNativeCallback =
    std::function<void(const std::string& ns, const std::string& name,
                       NativeFn fn, bool blocking)>;

class NativeLibraryLoader {
public:
    NativeLibraryLoader();
    ~NativeLibraryLoader();

    NativeLibraryLoader(const NativeLibraryLoader&) = delete;
    NativeLibraryLoader& operator=(const NativeLibraryLoader&) = delete;

    // Append a directory searched for native modules.
    void AddSearchDir(std::string dir);

    // Ensure the module implementing namespace `ns` is loaded. The first
    // call loads the shared library, validates its nlang_abi_version
    // probe (BEFORE any module code runs) and then runs its init; later
    // calls are no-ops. `registerFn` receives every function the module
    // registers. Throws std::runtime_error (with a message naming the
    // searched paths) when the module cannot be found, does not export
    // nlang_abi_version, has no nlang_native_init entry, or reports an
    // incompatible ABI version.
    void EnsureLoaded(const std::string& ns,
                      RegisterNativeCallback registerFn);

    // Directory containing the running executable (default search location).
    static std::string ExecutableDir();

    // Locate the module file for `ns` across the search dirs and the
    // built-in build-tree fallback; empty if not found. Pure query.
    std::string LocateModuleFile(const std::string& ns) const;

private:
    struct LoadedModule {
        std::string ns;
        void* handle = nullptr;
    };

    bool IsLoaded(const std::string& ns) const;

    //Built-in last-resort search: development hosts run from
    //<build>/src/tools/<tool>/<config> or <build>/tests/<config>; the
    //single authoritative module directory sits at <build>/native/<config>.
    //Walk up from the executable looking for native/<exe-dir-leaf>
    //(multi-config) or native (single-config). Installed packages have
    //no such directory — the fallback is a silent no-op there (the DLLs
    //ship beside the executable, searched first). Computed once.
    const std::vector<std::string>& FallbackDirs() const;

    //Indented listing of every consulted directory (explicit search
    //dirs, then the build-tree fallback) for load-failure diagnostics.
    std::string DescribeSearchedDirs() const;

    std::vector<std::string> m_searchDirs;
    std::vector<LoadedModule> m_loaded;
    mutable std::vector<std::string> m_fallbackDirs;
    mutable bool m_fallbackResolved = false;
};

} // namespace nlang
