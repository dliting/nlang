/*---
    IntrinsicsThread.cpp — K1 并发内在函数族：
      · thread.* 自由调用（129-133）：OP_CallFunc 内建臂派发，参数自
        callParamBase 槽 0 起（无 this）；
      · Mutex/Channel 方法（134-141）：接收者派发形态（槽 0 = this）。
    Slice 5 接入 yield/id，Slice 6 接入 spawn/join；其余在切片 7-9 逐个
    接入，此前统一「not implemented yet」基础异常（编译面已注册，行为
    面未到）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//Slices 7-9 land the real implementations; until then every
//not-yet-arrived intrinsic reports its surface name and throws.
[[noreturn]] static void ThrowThreadNotImplemented(const char* surface)
{
    throw std::runtime_error(
        std::string("NLang VM: ") + surface + " is not implemented yet");
}

bool VmExecutor::ExecuteIntrinsicThread(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    switch (intrinsicId)
    {
    case INTR_Thread_Yield:
        //Explicit cooperative yield: the same Preempted signal as quota
        //preemption — the carrier steals the frames, requeues, and the
        //resume re-enters RunFrames at the advanced pc. [[noreturn]].
        RequestPark(ParkSignal::Reason::Preempted);
    case INTR_Thread_Id:
    {
        int32_t id = m_spScheduler->CurrentContextId();
        std::memcpy(pResult, &id, sizeof(id));
        return true;
    }
    case INTR_Thread_Spawn:
        return IntrinsicSpawn(callParamBase, locals, pResult);
    case INTR_Thread_Join:
        return IntrinsicJoin(callParamBase, locals);
    //Registered but not yet implemented — each arrives in its slice
    //(sleep Slice 7, Mutex Slice 8, Channel Slice 9).
    case INTR_Thread_Sleep:
        ThrowThreadNotImplemented("thread.sleep");
    case INTR_Mutex_Ctor:
        ThrowThreadNotImplemented("Mutex constructor");
    case INTR_Mutex_Lock:
        ThrowThreadNotImplemented("Mutex.lock");
    case INTR_Mutex_Unlock:
        ThrowThreadNotImplemented("Mutex.unlock");
    case INTR_Mutex_TryLock:
        ThrowThreadNotImplemented("Mutex.tryLock");
    case INTR_Channel_Ctor:
        ThrowThreadNotImplemented("Channel constructor");
    case INTR_Channel_Send:
        ThrowThreadNotImplemented("Channel.send");
    case INTR_Channel_Recv:
        ThrowThreadNotImplemented("Channel.recv");
    case INTR_Channel_Close:
        ThrowThreadNotImplemented("Channel.close");
    default:
        return false;   //not ours — ExecuteIntrinsic reaches the
                        //unknown-id throw
    }
}

//thread.spawn(func<void> f) -> Thread. The root frame is built AT spawn
//(plan decision 6): the delegate's target resolves now (virtual handles
//bind the receiver's runtime class), the receiver rides slot 0 for bound
//handles, and the whole frame parks in the new context — the precise
//frame-local scan covers it from the first GC, no extra root storage.
bool VmExecutor::IntrinsicSpawn(uint16_t callParamBase, uint8_t* locals,
                                uint8_t* pResult)
{
    //Argument 0: the func<void> delegate value (RTK_Func heap handle).
    int32_t handleIdx;
    std::memcpy(&handleIdx, locals + callParamBase, sizeof(handleIdx));
    if (handleIdx <= 0
        || static_cast<size_t>(handleIdx) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(handleIdx)] != RTK_Func)
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null function value in thread.spawn");
    int32_t receiverIdx = 0;
    const CompiledFunction& callee = ResolveSpawnTarget(
        m_structHeap[static_cast<size_t>(handleIdx)], receiverIdx);

    const int ctxId = m_spScheduler->RegisterContext();
    ExecutionContext* ctx = m_spScheduler->FindMutable(ctxId);
    CallFrame root;
    root.localsBuf.assign(callee.localsSize, 0);
    if (receiverIdx != 0)
        std::memcpy(root.localsBuf.data(), &receiverIdx,
                    sizeof(receiverIdx));
    //Result threads to the context's own cell (deque-stable across later
    //registrations — see VmScheduler.h). Writes are ≤ kFrameSlotBytes
    //memcpys, so the int64_t cell is a valid uniform result cell.
    root.externalResult =
        reinterpret_cast<uint8_t*>(&ctx->resultCell);
    root.func = &callee;
    ctx->frames.push_back(std::move(root));

    int32_t threadObj = MintThreadObject(ctxId);
    ctx->threadObjHeapIdx = threadObj;
    std::memcpy(pResult, &threadObj, sizeof(threadObj));
    CheckGCSafepoint();
    return true;
}

//Validate the spawn delegate and resolve its target. Host-bound and
//intrinsic bodies have no bytecode frame to run — a spawned context of
//one would die instantly and confusingly; the resolver already rejects
//these shapes at compile time, this is the defense-in-depth backstop
//(same contract as delegate dispatch). Zero-argument contract: a free
//function takes no formals; a bound method takes exactly `this`.
const CompiledFunction& VmExecutor::ResolveSpawnTarget(
    const std::vector<int32_t>& handle, int32_t& receiverIdx)
{
    int funcIndex = ResolveDelegateTarget(handle);
    const CompiledFunction& callee
        = m_currModule->functions[static_cast<size_t>(funcIndex)];
    if (callee.isNative)
        throw std::runtime_error(
            "NLang VM: thread.spawn target cannot be a native function");
    if (callee.intrinsicId != INTR_None)
        throw std::runtime_error(
            "NLang VM: thread.spawn target cannot be an intrinsic function");
    receiverIdx = handle[1];
    if (callee.paramCount != (receiverIdx != 0 ? 1 : 0))
        throw std::runtime_error(
            "NLang VM: thread.spawn requires a zero-argument void function");
    return callee;
}

//Mint the Thread object for a registered context: __handle (slot 1,
//the first field) = context id. The context (with its frames) is
//already registered, so a GC at the caller's safepoint sees everything
//through the scheduler root arm.
int32_t VmExecutor::MintThreadObject(int ctxId)
{
    if (m_threadClassIdx < 0)
        throw std::runtime_error("NLang VM: Thread class not registered");
    int32_t threadObj = AllocClassOnHeap(
        static_cast<uint16_t>(m_threadClassIdx));
    //Write by index — AllocClassOnHeap may have reallocated m_structHeap.
    m_structHeap[static_cast<size_t>(threadObj)][1] = ctxId;
    return threadObj;
}

//thread.join(Thread t). Finished → return at once (idempotent; multiple
//joins of one target all return). Failed → rethrow the target's original
//exception object into the joiner's frame. Otherwise park: ParkForJoin
//records state+target, RequestPark throws the signal the carrier turns
//into the frame steal.
bool VmExecutor::IntrinsicJoin(uint16_t callParamBase, uint8_t* locals)
{
    //Argument 0: the Thread instance; __handle (slot 1) = context id.
    int32_t threadObjIdx;
    std::memcpy(&threadObjIdx, locals + callParamBase,
                sizeof(threadObjIdx));
    if (threadObjIdx <= 0
        || static_cast<size_t>(threadObjIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in thread.join");
    const int32_t targetId =
        m_structHeap[static_cast<size_t>(threadObjIdx)][1];
    const ExecutionContext* target = m_spScheduler->Find(targetId);
    if (!target)
        throw std::runtime_error(
            "NLang VM: thread.join target is not a live context");
    const int selfId = m_spScheduler->CurrentContextId();
    if (targetId == selfId)
        throw std::runtime_error("NLang VM: a thread cannot join itself");
    if (target->state == ContextState::Finished)
        return true;
    if (target->state == ContextState::Failed)
        ThrowJoinTargetFailure(target->failExcHeapIdx);
    m_spScheduler->ParkForJoin(selfId, targetId);
    RequestPark(ParkSignal::Reason::Join);   //[[noreturn]]
}

//Rethrow a failed join target's exception OBJECT (same heap idx — same
//message, same raise-time backtrace). Shared by the join intrinsic
//(target already Failed) and the resume path (RunFrames' entry throw).
[[noreturn]] void VmExecutor::ThrowJoinTargetFailure(int32_t excHeapIdx)
{
    if (excHeapIdx <= 0
        || static_cast<size_t>(excHeapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: join target failed without an exception object");
    throw NLangThrow(excHeapIdx,
                     ThrownMessageOf(excHeapIdx,
                                     "NLang VM: join target failed"));
}

} // namespace nlang
