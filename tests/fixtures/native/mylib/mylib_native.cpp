// Third-party native library fixture. Compiled to nlang_mylib.dll; the
// matching declarations live in the test-generated mylib.n. Demonstrates
// that a third-party namespace uses the same NativeHost ABI as the
// standard library (math/io/fs).

#include <nlang/vm/NativeHost.h>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace nlang::native;

static void Add(NativeHost* host, uint8_t* ret,
                const uint8_t* args, int argc) {
    (void)host; (void)argc;
    ReturnInt(ret, ArgInt(args, 0) + ArgInt(args, 1));
}

static void Mul(NativeHost* host, uint8_t* ret,
                const uint8_t* args, int argc) {
    (void)host; (void)argc;
    ReturnInt(ret, ArgInt(args, 0) * ArgInt(args, 1));
}

static void Greet(NativeHost* host, uint8_t* ret,
                  const uint8_t* args, int argc) {
    (void)argc;
    std::string who = ArgString(host, args, 0);
    ReturnString(host, ret, std::string("hello, ") + who);
}

// Self-path probe: returns the file path of the loaded copy of this
// very library. The extraction-cache tests call it to prove which
// copy answered — identical copies in the executable directory or the
// build tree's native directory would load just as well and fake the
// assertion.
static void Where(NativeHost* host, uint8_t* ret,
                  const uint8_t* args, int argc) {
    (void)args; (void)argc;
#if defined(_WIN32)
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&Where), &self);
    char buffer[MAX_PATH] = {};
    if (self)
        GetModuleFileNameA(self, buffer, MAX_PATH);
    ReturnString(host, ret, std::string(buffer));
#else
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(&Where), &info) && info.dli_fname)
        ReturnString(host, ret, std::string(info.dli_fname));
    else
        ReturnString(host, ret, std::string());
#endif
}

NLANG_DEFINE_NATIVE_INIT
{
    (void)regBlocking;
    reg(registry, "mylib", "add", &Add);
    reg(registry, "mylib", "mul", &Mul);
    reg(registry, "mylib", "greet", &Greet);
    reg(registry, "mylib", "where", &Where);
    return NLANG_HOST_ABI_VERSION;
}
