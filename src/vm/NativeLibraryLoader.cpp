#include "NativeLibraryLoader.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <dlfcn.h>
#  include <mach-o/dyld.h>
#else
#  include <dlfcn.h>
#endif

namespace nlang {

namespace fs = std::filesystem;

namespace {

void* OpenShared(const std::string& path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* ResolveSharedSymbol(void* handle, const char* symbol) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol));
#else
    return dlsym(handle, symbol);
#endif
}

void CloseShared(void* handle) {
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

std::string ModuleFileName(const std::string& ns) {
#if defined(_WIN32)
    return "nlang_" + ns + ".dll";
#elif defined(__APPLE__)
    return "libnlang_" + ns + ".dylib";
#else
    return "libnlang_" + ns + ".so";
#endif
}

// Context handed to a module's init as the opaque registry pointer; the
// registration trampoline restores it to forward each function to the
// caller's callback.
struct RegistrationContext {
    RegisterNativeCallback* callback;
};

void RegisterTrampoline(void* registry, const char* ns, const char* name,
                        NativeFn fn) {
    auto* ctx = static_cast<RegistrationContext*>(registry);
    (*ctx->callback)(ns ? ns : "", name ? name : "", fn);
}

} // namespace

NativeLibraryLoader::NativeLibraryLoader() = default;

NativeLibraryLoader::~NativeLibraryLoader() {
    // Unload in reverse load order. The owner must have dropped any
    // references to the registered functions before this point.
    for (auto it = m_loaded.rbegin(); it != m_loaded.rend(); ++it) {
        if (it->handle)
            CloseShared(it->handle);
    }
}

void NativeLibraryLoader::AddSearchDir(std::string dir) {
    m_searchDirs.push_back(std::move(dir));
}

bool NativeLibraryLoader::IsLoaded(const std::string& ns) const {
    for (const auto& module : m_loaded)
        if (module.ns == ns)
            return true;
    return false;
}

std::string NativeLibraryLoader::LocateModuleFile(
    const std::string& ns) const {
    const std::string fileName = ModuleFileName(ns);
    for (const auto& dir : m_searchDirs) {
        fs::path candidate = fs::path(dir) / fileName;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec))
            return candidate.string();
    }
    return std::string();
}

void NativeLibraryLoader::EnsureLoaded(
    const std::string& ns, RegisterNativeCallback registerFn) {
    if (IsLoaded(ns))
        return;

    const std::string path = LocateModuleFile(ns);
    if (path.empty()) {
        std::string searched;
        for (const auto& dir : m_searchDirs) {
            searched += "  ";
            searched += dir;
            searched += "\n";
        }
        throw std::runtime_error(
            "cannot find native module '" + ModuleFileName(ns)
            + "' for package '" + ns + "'; searched:\n" + searched);
    }

    void* handle = OpenShared(path);
    if (!handle)
        throw std::runtime_error("failed to load native module '" + path
                                 + "'");

    void* symbol = ResolveSharedSymbol(handle, "nlang_native_init");
    if (!symbol) {
        CloseShared(handle);
        throw std::runtime_error(
            "native module '" + path
            + "' does not export 'nlang_native_init'");
    }

    auto init = reinterpret_cast<NativeModuleInitFn>(symbol);
    RegistrationContext context{&registerFn};
    const int reportedAbi = init(&context, &RegisterTrampoline);
    if (static_cast<uint32_t>(reportedAbi) != NLANG_HOST_ABI_VERSION) {
        CloseShared(handle);
        throw std::runtime_error(
            "native module '" + path
            + "' reports incompatible ABI version "
            + std::to_string(reportedAbi) + "; host expects "
            + std::to_string(NLANG_HOST_ABI_VERSION));
    }

    m_loaded.push_back(LoadedModule{ns, handle});
}

std::string NativeLibraryLoader::ExecutableDir() {
#if defined(_WIN32)
    char buffer[MAX_PATH] = {};
    DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0)
        return std::string();
    return fs::path(buffer).parent_path().string();
#elif defined(__APPLE__)
    char buffer[1024];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0)
        return std::string();
    return fs::path(buffer).parent_path().string();
#else
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec)
        return std::string();
    return exe.parent_path().string();
#endif
}

} // namespace nlang
