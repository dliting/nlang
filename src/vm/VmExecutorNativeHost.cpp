/*---
VmExecutorNativeHost.cpp — VmExecutor's implementation of the public
NativeHost ABI: the function-table callbacks, lazy native-module loading
and the one-shot List<string> construction.

Native code (standard-library DLLs and third-party modules) accesses the VM
only through these callbacks; the mutable state (PRNG, string/list stores)
stays here, in the executor.
---*/
#include "VmExecutor.h"
#include "InputLineSource.h"
#include "LineSource.h"
#include "TokenView.h"
#include "VmExecutorSer.h"
#include "NativeLibraryLoader.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace nlang {

namespace {
//A native side holds NativeHost* equal to &vmNativeHost.c (the first
//member); restore the wrapper.
VmNativeHost* Wrap(NativeHost* self) {
    return reinterpret_cast<VmNativeHost*>(self);
}

//True for a real console — including a ConPTY-attached process, whose
//std handles are console handles by design. Pipes and files are not
//consoles. Windows asks GetConsoleMode rather than _isatty: _isatty
//answers "character device", which is also true for NUL, and
//`nvm x < NUL` at a console would then flash a prompt for input that
//can never arrive. POSIX isatty already draws the line correctly
//(/dev/null is a character device but not a tty).
bool IsConsoleStream(std::FILE* stream) {
#if defined(_WIN32)
    const HANDLE handle = reinterpret_cast<HANDLE>(
        _get_osfhandle(_fileno(stream)));
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD mode = 0;
    return GetConsoleMode(handle, &mode) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

//The interactive input prompt, same token nide's terminal draws in Line
//mode (TerminalWidget kLinePrompt). Precedent: REPLs (python, sqlite3)
//print their prompt from the runtime's stdio layer because that layer
//is the only one that knows a read is about to block; nide's run page
//is a passthrough ConPTY, so this write is what puts the prompt on its
//screen. Gated to two consoles: any pipe or redirection (scripted
//stdin, `nvm x > out.txt`) keeps both streams byte-exact for e2e.
constexpr const char* kInputPrompt = "> ";

void EmitInteractivePrompt() {
    if (!IsConsoleStream(stdin) || !IsConsoleStream(stdout))
        return;
    std::fputs(kInputPrompt, stdout);
    std::fflush(stdout);
}
} // namespace

void VmExecutor::InitNativeHost(VmNativeHost& host) {
    host.executor = this;
    host.stringScratch.clear();
    host.lineScratch.clear();
    host.c.abiVersion = NLANG_HOST_ABI_VERSION;
    host.c.getString = &VmExecutor::NativeGetString;
    host.c.newString = &VmExecutor::NativeNewString;
    host.c.newListString = &VmExecutor::NativeNewListString;
    host.c.writeOutput = &VmExecutor::NativeWriteOutput;
    host.c.writeError = &VmExecutor::NativeWriteError;
    host.c.readLine = &VmExecutor::NativeReadLine;
    host.c.readToken = &VmExecutor::NativeReadToken;
    host.c.readChar = &VmExecutor::NativeReadChar;
    host.c.hasInput = &VmExecutor::NativeHasInput;
    host.c.raiseException = &VmExecutor::NativeRaiseException;
    host.c.nextRandom = &VmExecutor::NativeNextRandom;
    host.c.seedRandom = &VmExecutor::NativeSeedRandom;
}

//--- NativeHost callbacks ---------------------------------------------------

const char* VmExecutor::NativeGetString(NativeHost* self, int32_t handle) {
    VmNativeHost* h = Wrap(self);
    h->stringScratch = h->executor->StrValCopy(handle);
    return h->stringScratch.c_str();
}

int32_t VmExecutor::NativeNewString(NativeHost* self, const char* utf8) {
    return Wrap(self)->executor->MintNewString(utf8 ? utf8 : "");
}

int32_t VmExecutor::NativeNewListString(NativeHost* self,
                                        const char* const* items,
                                        int count) {
    return Wrap(self)->executor->BuildListString(items, count);
}

void VmExecutor::NativeWriteOutput(NativeHost* self, const char* text) {
    VmExecutor* e = Wrap(self)->executor;
    const char* out = text ? text : "";
    if (e->m_pHostIo) {
        e->m_pHostIo->OnOutput(out);
    } else {
        std::fwrite(out, 1, std::strlen(out), stdout);
        std::fflush(stdout);
    }
}

void VmExecutor::NativeWriteError(NativeHost* self, const char* text) {
    VmExecutor* e = Wrap(self)->executor;
    const char* out = text ? text : "";
    if (e->m_pHostIo) {
        //A debug session has one merged output view — diagnostics
        //interleave with stdout there by design.
        e->m_pHostIo->OnOutput(out);
    } else {
        std::fwrite(out, 1, std::strlen(out), stderr);
        std::fflush(stderr);
    }
}

TokenView& VmExecutor::EnsureInputView() {
    if (!m_upInputView) {
        if (m_pHostIo) {
            //Embedder seam: the host owns the input conversation and
            //its prompt (nide's debug terminal draws one), so the
            //console cue stays uninstalled here — one prompt, one owner.
            m_upInputSource =
                std::make_unique<HostIoLineSource>(*m_pHostIo);
        } else {
            auto consoleSource = std::make_unique<InputLineSource>();
            //This is the one place the executor learns "the program is
            //about to wait for input": the line source cues exactly the
            //pulls that may block; interactive consoles answer with the
            //prompt, everything else stays silent (EmitInteractivePrompt).
            consoleSource->SetOnInputWait(&EmitInteractivePrompt);
            m_upInputSource = std::move(consoleSource);
        }
        m_upInputView = std::make_unique<TokenView>(*m_upInputSource);
    }
    return *m_upInputView;
}

const char* VmExecutor::NativeReadLine(NativeHost* self) {
    VmNativeHost* h = Wrap(self);
    VmExecutor* e = h->executor;
    std::string line;
    const InputReadStatus st = e->EnsureInputView().ReadLine(line);
    if (st == InputReadStatus::NoChannel)
        e->RaiseNlangException(e->m_ioExcClassIdx,
            "io.readLine: stdin is not available in this session.");
    if (st == InputReadStatus::Eof)
        return nullptr;   //end of input: io_native folds null into the
                          //"" sentinel — line-level reads never raise
    //Trailing '\r' was already stripped by the line source.
    h->lineScratch = std::move(line);
    return h->lineScratch.c_str();
}

const char* VmExecutor::NativeReadToken(NativeHost* self) {
    VmNativeHost* h = Wrap(self);
    VmExecutor* e = h->executor;
    std::string token;
    const InputReadStatus st = e->EnsureInputView().ReadToken(token);
    if (st == InputReadStatus::NoChannel)
        e->RaiseNlangException(e->m_ioExcClassIdx,
            "io.readToken: stdin is not available in this session.");
    if (st == InputReadStatus::Eof)
        e->RaiseNlangException(e->m_ioExcClassIdx,
            "io.readToken: input ended.");
    h->lineScratch = std::move(token);
    return h->lineScratch.c_str();
}

int VmExecutor::NativeReadChar(NativeHost* self, uint32_t* outChar) {
    VmExecutor* e = Wrap(self)->executor;
    uint32_t codePoint = 0;
    bool invalidUtf8 = false;
    const InputReadStatus st = e->EnsureInputView().ReadChar(
        codePoint, invalidUtf8);
    if (st == InputReadStatus::NoChannel)
        e->RaiseNlangException(e->m_ioExcClassIdx,
            "io.readChar: stdin is not available in this session.");
    if (st == InputReadStatus::Eof)
        e->RaiseNlangException(e->m_ioExcClassIdx,
            "io.readChar: input ended.");
    if (invalidUtf8)
        e->RaiseNlangExceptionBase("io.readChar: input is not valid UTF-8.");
    *outChar = codePoint;
    return 0;   //contract: 0 = ok (end of input raised above)
}

int VmExecutor::NativeHasInput(NativeHost* self) {
    VmExecutor* e = Wrap(self)->executor;
    //Predicate contract: never raises. A no-channel host latches
    //HasMore false on its first read (which raises); until then the
    //default host probe answers true — documented in io.n's comment.
    return e->EnsureInputView().HasInput() ? 1 : 0;
}

void VmExecutor::NativeRaiseException(NativeHost* self, int exceptionKind,
                                      const char* message) {
    VmExecutor* e = Wrap(self)->executor;
    int16_t classIdx = (exceptionKind == NEXC_IOException)
        ? e->m_ioExcClassIdx : e->m_exceptionClassIdx;
    e->RaiseNlangException(classIdx, message ? message : "");
}

uint32_t VmExecutor::NativeNextRandom(NativeHost* self) {
    return Wrap(self)->executor->m_rng();
}

void VmExecutor::NativeSeedRandom(NativeHost* self, int32_t seed) {
    Wrap(self)->executor->m_rng.seed(static_cast<uint32_t>(seed));
}

//--- List<string> construction ---------------------------------------------

int32_t VmExecutor::BuildListString(const char* const* items, int count) {
    if (m_listClassIdx < 0)
        throw std::runtime_error("NLang VM: List class not registered");
    int32_t listHeapIdx = AllocClassOnHeap(
        static_cast<uint16_t>(m_listClassIdx));
    int32_t listHandle = AllocListHandle();
    m_structHeap[static_cast<size_t>(listHeapIdx)]
        [kListHandleFieldOffset] = listHandle;
    auto& dst = m_listStore[listHandle - 1].elements;
    if (count > 0)
        dst.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const char* text = (items && items[i]) ? items[i] : "";
        dst.push_back(AllocBoxedValue(RTK_String, MintNewString(text)));
    }
    return listHeapIdx;
}

//--- Native module loading --------------------------------------------------

NativeLibraryLoader& VmExecutor::EnsureNativeLoader() {
    if (!m_upNativeLoader) {
        m_upNativeLoader = std::make_unique<NativeLibraryLoader>();
        //Default: native modules ship beside the executable.
        m_upNativeLoader->AddSearchDir(NativeLibraryLoader::ExecutableDir());
    }
    return *m_upNativeLoader;
}

void VmExecutor::AddNativeSearchDir(std::string dir) {
    EnsureNativeLoader().AddSearchDir(std::move(dir));
}

void VmExecutor::EnsureNativeAvailable(const std::string& name) {
    if (m_natives.find(name) != m_natives.end())
        return;
    //A qualified "ns.name" is satisfied by a loadable module; a bare name
    //is a host-registered native and is not resolved through a DLL.
    size_t dot = name.find('.');
    if (dot == std::string::npos)
        return;
    std::string ns = name.substr(0, dot);
    NativeLibraryLoader& loader = EnsureNativeLoader();
    loader.EnsureLoaded(ns,
        [this](const std::string& modNs, const std::string& fnName,
               NativeFn fn) {
            m_natives[modNs + "." + fnName] = fn;
        });
}

} // namespace nlang
