/*---
VmExecutorNativeHostText.cpp — the text face of the NativeHost ABI: the
string-interning callbacks (getString/newString/newListString) and the
console conversation (writeOutput/writeError plus the readLine/readToken/
readChar/hasInput input family).

These are the entry points through which native code (standard-library
DLLs and third-party modules) exchanges text with the VM. The byte length
is the content boundary (ABI v13), so a NUL byte is ordinary string
content. The function-table wiring, the native registration tables, the
CallNative bridge and the module-state teardown stay in
VmExecutorNativeHost.cpp.

2026-11-15 source-size split: moved out of VmExecutorNativeHost.cpp,
whose ABI-13 byte-length signatures pushed it past the size guard
(zero behavior change).
---*/
#include "VmExecutor.h"
#include "VmExecutorNativeHost.h"
#include "InputLineSource.h"
#include "LineSource.h"
#include "TokenView.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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

//v13: one native string payload. The byte length is the content boundary
//(a NUL byte is ordinary string content), and a null pointer or a
//non-positive length is the empty payload — defensive for a caller that
//skips the null check its contract allows.
std::string_view Content(const char* text, int64_t len) {
    return (text != nullptr && len > 0)
        ? std::string_view(text, static_cast<size_t>(len))
        : std::string_view();
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

//--- NativeHost callbacks ---------------------------------------------------
//K8: every callback locks at its own touch face — store access rides
//the domain locks inside the VM helpers (creation, list build, exception
//build), while host-boundary resources (console conversation, RNG,
//root registries) take their dedicated leaf mutexes. The two
//scheduler-facing callbacks keep the store lock (defined in
//VmExecutorNativeCallbacks.cpp).

const char* VmExecutor::NativeGetString(NativeHost* self, int32_t handle,
                                        int64_t* outLen) {
    VmNativeHost* h = Wrap(self);
    //K8: lock-free — the copy walks the string store, which flattens
    //and interns under its own maintenance lock.
    h->stringScratch = h->executor->StrValCopy(handle);
    //v13: the byte length is the content boundary; a NUL byte inside the
    //sequence is content, and the c_str() terminator stays for C readers.
    if (outLen)
        *outLen = static_cast<int64_t>(h->stringScratch.size());
    return h->stringScratch.c_str();
}

int32_t VmExecutor::NativeNewString(NativeHost* self, const char* utf8,
                                    int64_t len) {
    VmExecutor* e = Wrap(self)->executor;
    //Native-creator gate: creation and root push are one indivisible span
    //against a collection led by another carrier (contract in
    //VmExecutor.h) — this thread may sit outside the rendezvous
    //denominator (blocking-native arm), and between the slot handout
    //and the root push the object is reachable from no root. Shared
    //lock: concurrent creators coexist, only the collector is exclusive.
    NativeCreationGate creationGate(*e);
    //K8: lock-free — creating self-locks; rooting rides the registry
    //mutex inside.
    //Ruling D27: the native boundary keeps 0 = null (the ABI's existing
    //null-handle convention), so the host's string-producing API never
    //hands out 0. Interning "" returns the internal slot 0 (ruling D26),
    //so empty content gets a fresh non-interned empty object instead —
    //equality still holds (a non-interned "" compares by content).
    //v13: `len` bytes at utf8; a null pointer or a non-positive length is
    //empty content (a negative length is defensive, never a slice run).
    const bool hasContent = (utf8 != nullptr) && (len > 0);
    int32_t handle = 0;
    if (hasContent)
        handle = e->CreateNewString(
            std::string(utf8, static_cast<size_t>(len)));
    else
        handle = e->AllocStringObj();   //fresh Flat "" (non-interned)
    //White-box seam (SetCreationRootGapMsForTests): hold the creation-to-root
    //window open so a test can drive a real collection through it.
    const int gapMs = e->m_creationRootGapMs.load(std::memory_order_relaxed);
    if (gapMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
    //Root across the blocking section: the native may hold this handle
    //for a while before writing it into ret, while other carriers GC.
    e->RootCreatedNativeHandle(handle);
    return handle;
}

int32_t VmExecutor::NativeNewListString(NativeHost* self,
                                        const char* const* items,
                                        const int64_t* lens, int count) {
    VmExecutor* e = Wrap(self)->executor;
    //Native-creator gate (creation+root indivisible; contract in VmExecutor.h).
    NativeCreationGate creationGate(*e);
    //K8: lock-free — the list build holds the Container domain
    //internally; rooting rides the registry mutex.
    const int32_t listHeapIdx = e->BuildListString(items, lens, count);
    //The list keeps its elements (MarkListElements); rooting the list
    //covers the whole created graph.
    e->RootCreatedNativeHandle(listHeapIdx);
    return listHeapIdx;
}

void VmExecutor::NativeWriteOutput(NativeHost* self, const char* text,
                                   int64_t len) {
    VmExecutor* e = Wrap(self)->executor;
    //K8: host IO callbacks are not promised thread-safe — the console
    //output mutex keeps whole-line writes whole.
    std::lock_guard<std::mutex> outGuard(e->m_consoleOutMutex);
    //v13: `len` bytes are the payload; the IHostIo seam already travels
    //by length, so a NUL byte reaches the host verbatim.
    const std::string_view out = Content(text, len);
    if (e->m_pHostIo) {
        e->m_pHostIo->OnOutput(out);
    } else {
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
    }
}

void VmExecutor::NativeWriteError(NativeHost* self, const char* text,
                                  int64_t len) {
    VmExecutor* e = Wrap(self)->executor;
    std::lock_guard<std::mutex> outGuard(e->m_consoleOutMutex);
    const std::string_view out = Content(text, len);
    if (e->m_pHostIo) {
        //Separate error channel (extension point ④): hosts with a
        //merged view keep it via IHostIo::OnError's OnOutput default.
        e->m_pHostIo->OnError(out);
    } else {
        std::fwrite(out.data(), 1, out.size(), stderr);
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

const char* VmExecutor::NativeReadLine(NativeHost* self, int64_t* outLen) {
    VmNativeHost* h = Wrap(self);
    VmExecutor* e = h->executor;
    if (outLen)
        *outLen = 0;   //both raising and end-of-input outcomes carry no bytes
    std::string line;
    InputReadStatus st;
    {
        //K8: the input view is one sequential conversation (lazy init
        //+ cursor state); the console input mutex serializes readers.
        //Blocking at a prompt is fine — output has its own mutex.
        std::lock_guard<std::mutex> inGuard(e->m_consoleInMutex);
        st = e->EnsureInputView().ReadLine(line);
    }
    if (st == InputReadStatus::NoChannel)
        e->RaiseNlangException(e->m_ioExcClassIdx,
            "io.readLine: stdin is not available in this session.");
    if (st == InputReadStatus::Eof)
        return nullptr;   //end of input: io_native folds null into the
                          //"" sentinel — line-level reads never raise
    //Trailing '\r' was already stripped by the line source. A NUL byte
    //inside the line stayed in the buffer: it is content, not a boundary.
    h->lineScratch = std::move(line);
    if (outLen)
        *outLen = static_cast<int64_t>(h->lineScratch.size());
    return h->lineScratch.c_str();
}

const char* VmExecutor::NativeReadToken(NativeHost* self, int64_t* outLen) {
    VmNativeHost* h = Wrap(self);
    VmExecutor* e = h->executor;
    std::string token;
    InputReadStatus st;
    {
        std::lock_guard<std::mutex> inGuard(e->m_consoleInMutex);
        st = e->EnsureInputView().ReadToken(token);
    }
    if (st == InputReadStatus::NoChannel)
        e->RaiseNlangException(e->m_ioExcClassIdx,
            "io.readToken: stdin is not available in this session.");
    if (st == InputReadStatus::Eof)
        e->RaiseNlangException(e->m_ioExcClassIdx,
            "io.readToken: input ended.");
    h->lineScratch = std::move(token);
    if (outLen)
        *outLen = static_cast<int64_t>(h->lineScratch.size());
    return h->lineScratch.c_str();
}

int VmExecutor::NativeReadChar(NativeHost* self, uint32_t* outChar) {
    VmExecutor* e = Wrap(self)->executor;
    uint32_t codePoint = 0;
    bool invalidUtf8 = false;
    InputReadStatus st;
    {
        std::lock_guard<std::mutex> inGuard(e->m_consoleInMutex);
        st = e->EnsureInputView().ReadChar(codePoint, invalidUtf8);
    }
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
    std::lock_guard<std::mutex> inGuard(e->m_consoleInMutex);
    return e->EnsureInputView().HasInput() ? 1 : 0;
}

} // namespace nlang
