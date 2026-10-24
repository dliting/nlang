/*---
VmExecutorNativeHost.cpp — VmExecutor's implementation of the public
NativeHost ABI: the function-table callbacks, the native call bridge
(registration, CallNative dispatch and the K2 blocking-native protocol),
lazy native-module loading and the one-shot List<string> construction.

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
#include <exception>
#include <iterator>
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
    host.c.getArrayView = &VmExecutor::NativeGetArrayView;
    host.c.newArray = &VmExecutor::NativeNewArray;
}

//--- NativeHost callbacks ---------------------------------------------------
//K2 slice 4 (§2.5 re-entrant callback GIL): every callback that touches
//VM state takes the GIL through GilEnsureGuard. Inside a carrier window
//or a non-blocking native the lock is already held and the guard is a
//no-op; inside a blocking native (or from a host thread) it acquires.

const char* VmExecutor::NativeGetString(NativeHost* self, int32_t handle) {
    VmNativeHost* h = Wrap(self);
    GilEnsureGuard gil(*h->executor);
    h->stringScratch = h->executor->StrValCopy(handle);
    return h->stringScratch.c_str();
}

int32_t VmExecutor::NativeNewString(NativeHost* self, const char* utf8) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    const int32_t handle = e->MintNewString(utf8 ? utf8 : "");
    //Root across the blocking section: the native may hold this handle
    //for a while before writing it into ret, while other carriers GC.
    e->RootMintedNativeHandle(handle);
    return handle;
}

int32_t VmExecutor::NativeNewListString(NativeHost* self,
                                        const char* const* items,
                                        int count) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    const int32_t listHeapIdx = e->BuildListString(items, count);
    //The list keeps its elements (MarkListElements); rooting the list
    //covers the whole minted graph.
    e->RootMintedNativeHandle(listHeapIdx);
    return listHeapIdx;
}

void VmExecutor::NativeWriteOutput(NativeHost* self, const char* text) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
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
    GilEnsureGuard gil(*e);
    const char* out = text ? text : "";
    if (e->m_pHostIo) {
        //Separate error channel (extension point ④): hosts with a
        //merged view keep it via IHostIo::OnError's OnOutput default.
        e->m_pHostIo->OnError(out);
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
    GilEnsureGuard gil(*e);
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
    GilEnsureGuard gil(*e);
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
    GilEnsureGuard gil(*e);
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
    GilEnsureGuard gil(*e);
    //Predicate contract: never raises. A no-channel host latches
    //HasMore false on its first read (which raises); until then the
    //default host probe answers true — documented in io.n's comment.
    return e->EnsureInputView().HasInput() ? 1 : 0;
}

//--- Native call bridge (K2 slice 4, §2.5) -----------------------------------
//The registration tables and the dispatch into native code, including the
//blocking protocol for natives that may wait on the outside world.

//K2 slice 4 thread-locals (declared in VmExecutor.h).
thread_local int VmExecutor::t_blockingNativeDepth = 0;
thread_local std::vector<VmExecutor::NativeCallRoot>*
    VmExecutor::t_currentNativeRootFrame = nullptr;
thread_local VmExecutor::GilHolder* VmExecutor::t_gilOwner = nullptr;

//Compiler-side sanity ceiling for call parameter counts (SnData.h
//kMaxFuncParams — the VM must not include compiler AST headers, so the
//value is mirrored here and tied by this comment).
constexpr uint32_t kMaxNativeStagedParams = 64;

void VmExecutor::RegisterNative(const std::string& name, NativeFn fn)
{
    if (m_natives.count(name))
        throw std::runtime_error(
            "NLang VM: duplicate native registration: " + name);
    m_natives[name] = NativeEntry{fn, /*blocking=*/false};
}

void VmExecutor::RegisterNativeBlocking(const std::string& name,
                                        NativeFn fn)
{
    if (m_natives.count(name))
        throw std::runtime_error(
            "NLang VM: duplicate native registration: " + name);
    m_natives[name] = NativeEntry{fn, /*blocking=*/true};
}

void VmExecutor::CallNative(const CompiledFunction& callee,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    //Extension point ⑤: the host dispatch hook answers first — host
    //registrations win over same-named DLL natives and never trigger a
    //DLL load attempt (that is why this precedes EnsureNativeAvailable).
    //K2 slice 4 (§2.5): host code is unauditable, so the hook always
    //runs through the blocking protocol (staged cells + GIL release);
    //a declined call merely wastes the (state-neutral) dance.
    if (m_pHostFunctions) {
        const CompiledFunction* cal = &callee;
        if (RunBlockingNative(callee, callParamBase, locals, pResult,
                [this, cal](const uint8_t* args, uint32_t argc,
                            uint8_t* ret) {
                    return m_pHostFunctions->Call(*cal, args, argc, ret);
                }))
            return;
    }
    EnsureNativeAvailable(callee.name);
    auto it = m_natives.find(callee.name);
    if (it == m_natives.end())
        throw std::runtime_error(
            "NLang VM: native function not registered: " + callee.name);
    VmNativeHost host;
    InitNativeHost(host);
    if (it->second.blocking) {
        NativeFn fn = it->second.fn;
        RunBlockingNative(callee, callParamBase, locals, pResult,
            [&host, fn](const uint8_t* args, uint32_t argc,
                        uint8_t* ret) {
                fn(&host.c, ret, args, static_cast<int>(argc));
                return true;
            });
        return;
    }
    it->second.fn(&host.c, pResult, locals + callParamBase,
                  callee.paramCount);
}

//Steal the carrier's frame stack into the context and mark it
//BlockedNative (invisible to PopNextRunnable, still GC-scanned).
void VmExecutor::ParkFramesForBlockingNative(ExecutionContext* ctx)
{
    ctx->frames.insert(ctx->frames.end(),
        std::make_move_iterator(m_callStack.begin()),
        std::make_move_iterator(m_callStack.end()));
    m_callStack.clear();
    m_spScheduler->MarkBlockedNative(ctx->id);
}

//Restore exactly the stolen frames (a re-entrant host callback may
//have parked more under this one) and mark the context Runnable-held
//(no requeue: the carrier resumes in place; FIFO governs queue
//members only).
void VmExecutor::RestoreStolenFrames(ExecutionContext* ctx,
                                     size_t stolenDepth)
{
    m_callStack.assign(
        std::make_move_iterator(ctx->frames.end()
            - static_cast<std::ptrdiff_t>(stolenDepth)),
        std::make_move_iterator(ctx->frames.end()));
    ctx->frames.erase(ctx->frames.end()
        - static_cast<std::ptrdiff_t>(stolenDepth), ctx->frames.end());
    m_spScheduler->MarkRunnableHeld(ctx->id);
}


//K2 slice 4 (§2.5) — the blocking-native arm. Stages args and the result
//cell onto the carrier's C++ stack (the native sees only staging — no
//concurrent read channel into frame storage remains), steals the frame
//stack into the context (BlockedNative; m_callStack empty for the whole
//GIL release keeps INV-2 true mid-window), releases the GIL through the
//thread's current holder (window guard or a re-entrant callback's ensure
//guard — nesting works either way), runs the native, reacquires, and
//unwinds the section's root frame UNDER the lock (NativeRootFrameGuard):
//while the GIL is away the roots stay armed, since a GC another carrier
//runs before we get the lock has to see the staged cells (the native may
//already have written its result into them — retracting before
//reacquiring would open a collect-the-result window). Stop decides
//between ShutdownSignal passthrough (run over: frames stay parked, no
//result consumption) and restore. Exceptions from the native
//(raiseException's NLangThrow, or a host hook escaping despite the
//contract) are held and rethrown AFTER the restore, so RunFrames'
//handler walk sees the full frame stack.
bool VmExecutor::RunBlockingNative(const CompiledFunction& callee,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
    const BlockingInvoke& invoke)
{
    const uint32_t paramCount = callee.paramCount;
    if (paramCount > kMaxNativeStagedParams)
        throw std::runtime_error("NLang VM: native parameter count "
                                 "exceeds the staging ceiling");
    ExecutionContext* ctx = m_spScheduler->FindMutable(
        m_spScheduler->CurrentContextId());
    if (!ctx) {
        //Embed by-name bridge outside a run: run holding the GIL.
        return invoke(locals + callParamBase, paramCount, pResult);
    }
    //Before arming roots: a throw past here must not leave root cells.
    GilHolder* owner = t_gilOwner;
    if (!owner)
        throw std::runtime_error("NLang VM: no GIL holder for blocking native");
    uint8_t stagedArgs[kMaxNativeStagedParams * kFrameSlotBytes];
    if (paramCount)
        std::memcpy(stagedArgs, locals + callParamBase,
                    static_cast<size_t>(paramCount) * kFrameSlotBytes);
    uint8_t stagedRet[kFrameSlotBytes] = {0};
    //The staged cells stay rooted across the GIL release and mint
    //roots land in the guard's frame (see the class comment).
    NativeRootFrameGuard rootGuard(*this, stagedArgs, paramCount,
        stagedRet);
    const size_t stolenDepth = m_callStack.size();
    ParkFramesForBlockingNative(ctx);
    owner->Release();
    std::exception_ptr nativeExc;
    bool handled = true;
    try {
        handled = invoke(stagedArgs, paramCount, stagedRet);
    } catch (...) {
        nativeExc = std::current_exception();
    }
    owner->Acquire();
    //The guard destructs before either exit below: stagedArgs/
    //stagedRet live on this C++ stack, so their roots must die with
    //it. On stop the frames stay parked; m_callStack is empty (INV-2).
    if (m_spScheduler->StopRequested())
        throw ShutdownSignal();
    RestoreStolenFrames(ctx, stolenDepth);
    if (nativeExc)
        std::rethrow_exception(nativeExc);
    std::memcpy(pResult, stagedRet, kFrameSlotBytes);
    return handled;
}

//K2 slice 4: mint rooting — see NativeCallRoot. Only roots inside a
//blocking section on this thread; callbacks on foreign threads stay
//unrooted (the ABI contract keeps callback results on the calling
//thread for the duration of the call).
void VmExecutor::RootMintedNativeHandle(int32_t handle)
{
    if (t_blockingNativeDepth > 0 && handle != 0
        && t_currentNativeRootFrame)
        t_currentNativeRootFrame->push_back({nullptr, 0, handle});
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
               NativeFn fn, bool blocking) {
            m_natives[modNs + "." + fnName] = NativeEntry{fn, blocking};
        });
}

} // namespace nlang
