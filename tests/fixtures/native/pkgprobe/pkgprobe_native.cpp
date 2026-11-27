// Package-native probe fixture: the native side of "pkgprobe".
// Compiled to nlang_pkgprobe.dll; the matching declarations live in
// pkgprobe.n. value() answers a fixed constant so a corpus run can
// check behavior, and where() answers the loaded copy's own file path
// so a test can prove the package cache copy answered the call.
//
// The library also records its load into a mark file (path from the
// PKGPROBE_ATTACH_MARK environment variable) when the operating system
// maps it into a process. That separates "the module is loaded" (mark
// present) from "the file was merely extracted to the cache" (cache
// copy present, mark absent) — the extraction cache writes files
// without loading them, and the not-imported arm of the corpus relies
// on exactly that difference.

#include <nlang/vm/NativeHost.h>
#include <string>

#if defined(_WIN32)
#include <windows.h>

static void WriteAttachMark() {
    char markPath[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableA(
        "PKGPROBE_ATTACH_MARK", markPath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;  //no mark requested (or a bad path): not an error
    HANDLE file = CreateFileA(markPath, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, "attached\n", 9, &written, nullptr);
    CloseHandle(file);
}

extern "C" BOOL APIENTRY DllMain(HINSTANCE, DWORD reason, LPVOID) {
    //Loader-lock discipline: kernel32 file calls only, no CRT init, no
    //library loads.
    if (reason == DLL_PROCESS_ATTACH)
        WriteAttachMark();
    return TRUE;
}
#else
#include <dlfcn.h>

__attribute__((constructor)) static void WriteAttachMark() {
    const char* markPath = getenv("PKGPROBE_ATTACH_MARK");
    if (!markPath || !*markPath)
        return;
    FILE* mark = fopen(markPath, "w");
    if (!mark)
        return;
    fputs("attached\n", mark);
    fclose(mark);
}
#endif

using namespace nlang::native;

static void Value(NativeHost* host, uint8_t* ret,
                  const uint8_t* args, int argc) {
    (void)host; (void)args; (void)argc;
    ReturnInt(ret, 7341);
}

// Self-path probe: answers the file path of the loaded copy of this
// very library (the same probe the mylib fixture carries).
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
    reg(registry, "pkgprobe", "value", &Value);
    reg(registry, "pkgprobe", "where", &Where);
    return NLANG_HOST_ABI_VERSION;
}
