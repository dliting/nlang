/*---
VmExecutorNativeHost.cpp — VmExecutor's implementation of the public
NativeHost ABI: the function-table wiring, the native call bridge
(registration, CallNative dispatch and the K2 blocking-native protocol)
and the destructor/module-state teardown.

Native code (standard-library DLLs and third-party modules) accesses the VM
only through these callbacks; the mutable state (PRNG, string/list stores)
stays here, in the executor.

K6 Task 2 (2026-10-22): the blocking-native arm (RunBlockingNative and
its park/restore/create helpers) moved to VmExecutorBlockingNative.cpp.

2026-11-12 source-size split: lazy native-module loading and the one-shot
List<string> construction moved to VmExecutorNativeLoader.cpp.

2026-11-15 source-size split: the string-interning and console callbacks
moved to VmExecutorNativeHostText.cpp (the ABI-13 byte-length signatures
pushed this file past the size guard).
---*/
#include "VmExecutor.h"
#include "VmExecutorSer.h"
#include "NativeLibraryLoader.h"

#include <cstring>
#include <exception>
#include <string>

namespace nlang {

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
    host.c.newArrayShaped = &VmExecutor::NativeNewArrayShaped;
    host.c.newArrayView = &VmExecutor::NativeNewArrayView;
    //v11: IO parking, module state, close-path wakes, context id, work
    //pool (contracts in NativeHost.h; definitions in
    //VmExecutorNativeCallbacks.cpp).
    host.c.nowMs = &VmExecutor::NativeNowMs;
    host.c.parkForIo = &VmExecutor::NativeParkForIo;
    host.c.moduleState = &VmExecutor::NativeModuleState;
    host.c.wakeIoWaiters = &VmExecutor::NativeWakeIoWaiters;
    host.c.currentContextId = &VmExecutor::NativeCurrentContextId;
    host.c.submitWork = &VmExecutor::NativeSubmitWork;
    //v12: receiver type-argument reads (contracts in NativeHost.h;
    //definitions in VmExecutorNativeCallbacks.cpp). The receiver index
    //rides the call-local wrapper (CallNative fills it).
    host.c.typeArgCount = &VmExecutor::NativeTypeArgCount;
    host.c.typeArgKind = &VmExecutor::NativeTypeArgKind;
    host.c.typeArgName = &VmExecutor::NativeTypeArgName;
}

//K8: host-held exception rethrow resolution — see the header comment.
//Same family as NativeRaiseException (exception + unwind rooting, both in
//VmExecutorNativeCallbacks.cpp); runs under the blocking-native protocol
//(embed host hooks), so the root lands in the call's root set and leaves
//with it.
int32_t VmExecutor::ExceptionRethrowTarget(const std::string& exceptionClass,
                                           const std::string& message,
                                           int32_t maybeLiveHeapIdx) {
    //K8: lock-free entry — the probe reads heap slots (benign under
    //the sharing model), the rebuild self-locks (Container), and the
    //rooting rides the registry mutex.
    //Native-creator gate (contract in VmExecutor.h): probe/rebuild and
    //the root push must be atomic against a collection — an existing
    //object can pass the liveness probe and still be swept before the
    //root lands.
    NativeCreationGate creationGate(*this);
    //Probe the recorded index: kind intact, same class, same message
    //content — a slot recycled by another instance fails one of the
    //three and falls through to the re-creation. cell[0] = classIdx,
    //cell[1] = message handle (the BuildNlangException layout).
    if (maybeLiveHeapIdx > 0
            && static_cast<size_t>(maybeLiveHeapIdx) < m_slotKinds.size()
            && m_slotKinds[static_cast<size_t>(maybeLiveHeapIdx)] == RTK_Class) {
        const StructSlot& slot =
            m_structHeap[static_cast<size_t>(maybeLiveHeapIdx)];
        const int32_t recordedClass = m_currModule->FindClass(exceptionClass);
        const bool sameClass = slot.size() > 1 && slot[0] == recordedClass;
        const bool sameMessage = slot[1] > 0
            && StrValCopy(slot[1]) == message;
        if (sameClass && sameMessage) {
            RootCreatedNativeHandle(maybeLiveHeapIdx);
            return maybeLiveHeapIdx;
        }
    }
    int classIdx = m_currModule->FindClass(exceptionClass);
    if (classIdx < 0)
        classIdx = m_exceptionClassIdx;   //defensive: unknown name → base
    const int32_t heapIdx = BuildNlangException(
        static_cast<int16_t>(classIdx), message);
    RootCreatedNativeHandle(heapIdx);
    return heapIdx;
}


//--- Native call bridge (K2 slice 4, §2.5) -----------------------------------
//The registration tables and the dispatch into native code; the blocking
//protocol itself lives in VmExecutorBlockingNative.cpp (K6 Task 2).

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
    CallNativeDispatch(callee, callParamBase, locals, pResult);
    //Ruling D27: one boundary check after dispatch covers every arm
    //(host hook, blocking DLL, direct DLL).
    CheckNativeStringResult(callee, pResult);
}

//Ruling D27: a declared-string native result lives in the ABI's
//handle-or-null domain. 0 = the native produced no string — a contract
//violation, not the empty string (a silent "" would hide the producer
//bug); a non-live handle is a producer bug whose slot may already
//hold recycled content, so reading it would decode an arbitrary
//value. Both are host-level errors naming the function; internally
//handle 0 IS the empty string (ruling D26), and the host's
//string-producing API (newString) never hands out 0.
void VmExecutor::CheckNativeStringResult(const CompiledFunction& callee,
                                         const uint8_t* pResult) {
    if (callee.returnTypeKind != RTK_String)
        return;
    int32_t handle = 0;
    std::memcpy(&handle, pResult, sizeof(handle));
    if (handle == 0)
        throw std::runtime_error("NLang VM: native function '" +
                                 callee.name + "' returned null string");
    if (!IsLiveStringHandle(handle))
        throw std::runtime_error(
            "NLang VM: native function '" + callee.name +
            "' returned invalid string handle " + std::to_string(handle));
}

void VmExecutor::CallNativeDispatch(const CompiledFunction& callee,
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
        //K7: the blocking protocol hands the GIL over through the
        //thread's current holder — the guard supplies that holder
        //once the dispatch window is gone.
        GilEnsureGuard g(*this);
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
    FillNativeReceiver(host, callee, callParamBase, locals);
    if (it->second.blocking) {
        //K8: the blocking arm keeps the store lock — the protocol
        //stages roots and parks frames in executor storage and hands
        //the lock to the native through the thread's current holder.
        GilEnsureGuard g(*this);
        NativeFn fn = it->second.fn;
        RunBlockingNative(callee, callParamBase, locals, pResult,
            [&host, fn](const uint8_t* args, uint32_t argc,
                        uint8_t* ret) {
                fn(&host.c, ret, args, static_cast<int>(argc));
                return true;
            });
        return;
    }
    //K8 direct arm: lock-free — the native table is read-only during a
    //run and the host context is call-local; every store the native
    //touches goes through the self-guarded InitNativeHost callbacks.
    it->second.fn(&host.c, pResult, locals + callParamBase,
                  callee.paramCount);
}

//v12: the receiver travels in slot 0 of a native instance method's
//argument area. The record's isInstanceMethod flag (the compiler's
//HasThisReceiver verdict, serialized v2.16) is the discriminator —
//never a name-shape guess; isInstanceMethod implies !isStatic.
void VmExecutor::FillNativeReceiver(VmNativeHost& host,
    const CompiledFunction& callee, uint16_t callParamBase,
    uint8_t* locals) {
    host.receiverHeapIdx = 0;
    if (callee.isInstanceMethod && callee.paramCount > 0)
        std::memcpy(&host.receiverHeapIdx, locals + callParamBase,
                    sizeof(host.receiverHeapIdx));
}

//v11 R8: the destructor is defined here (moved from VmExecutor.cpp,
//which sits at the source-size budget) so it sits with the teardown it
//drives. Module-state dtors run in the destructor BODY — before member
//teardown destroys the scheduler (whose destructor joins the work
//pool): a net-module dtor cv-waits on jobs still queued in that pool,
//so the pool must outlive the dtors. Carriers are already joined (the
//run ended before the executor dies), satisfying the R8 precondition.
VmExecutor::~VmExecutor() {
    TeardownModuleStates();
}

void VmExecutor::TeardownModuleStates() {
    std::lock_guard<std::mutex> g(m_moduleStateMutex);
    for (auto& entry : m_moduleStates) {
        if (entry.second.first && entry.second.second)
            entry.second.second(entry.second.first);
    }
    m_moduleStates.clear();
}

} // namespace nlang
