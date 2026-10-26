/*---
    IntrinsicsThread.cpp — K1 并发内在函数族：
      · thread.* 自由调用（129-133）：经 OP_CallFunc 的内建函数分支
        派发，参数自 callParamBase 槽 0 起（无 this）；
      · Mutex/Channel 方法（134-141）：接收者派发形态（槽 0 = this）。
    Slice 5 接入 yield/id，Slice 6 接入 spawn/join，Slice 7 接入 sleep，
    Slice 8 接入 Mutex 全族，Slice 9 接入 Channel 全族——K1 内在函数族
    至此全部落地；Slice 10 补 spawn 的载体在飞门（无载体窗口拒绝）。
    K3 接入 Future 族（isDone 先行，get 随任务 2）：spawn 依委托返回
    种类铸造 Thread 或 Future 句柄。
    K8：家族在分派处拆锁——thread.* 与 Future 臂持调度器域锁，Mutex 臂
    落 Sync 域（两阶段停泊握手），Channel 臂同形（IntrinsicsChannel.cpp）。
---*/
#include "VmExecutor.h"
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace nlang {

bool VmExecutor::ExecuteIntrinsicThread(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    //K8: the family splits at the dispatch. thread.* arms touch
    //scheduler state and heap minting only — they keep the scheduler
    //domain lock (whole-body; a park's suspension unwind destroys the
    //guard first). The handle methods forward below: Future arms are
    //scheduler-state too, while the Mutex/Channel arms own the Sync
    //domain over the side tables and must never hold the scheduler
    //lock across their park/wake calls (the K8 domain-lock invariant).
    switch (intrinsicId)
    {
    case INTR_Thread_Yield:
    case INTR_Thread_Id:
    case INTR_Thread_Spawn:
    case INTR_Thread_Join:
    case INTR_Thread_Sleep:
        break;
    default:
        return ExecuteIntrinsicHandleMethods(intrinsicId, callParamBase,
                                             locals, pResult);
    }
    GilEnsureGuard g(*this);
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
    case INTR_Thread_Sleep:
        return IntrinsicSleep(callParamBase, locals);
    default:
        return false;   //unreachable — filtered above
    }
}

//The receiver-dispatched concurrency builtins (slot 0 = the instance):
//Mutex (Slice 8), Channel (Slice 9), Future (K3). An unknown id returns
//false — ExecuteIntrinsic reaches the unknown-id throw.
bool VmExecutor::ExecuteIntrinsicHandleMethods(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    //K8: Future arms read registry state and park through the join path
    //— scheduler domain, same lock discipline as thread.* above.
    switch (intrinsicId)
    {
    case INTR_Future_IsDone:
    case INTR_Future_Get:
    {
        GilEnsureGuard g(*this);
        return intrinsicId == INTR_Future_IsDone
            ? IntrinsicFutureIsDone(callParamBase, locals, pResult)
            : IntrinsicFutureGet(callParamBase, locals, pResult);
    }
    default:
        return ExecuteIntrinsicSync(intrinsicId, callParamBase, locals,
                                    pResult);
    }
}

//K8: the Mutex/Channel family entry. The arms run their structural
//sections under the Sync domain and their scheduler calls between the
//sections; NLang raises inside a section defer (a reverse acquisition
//under Sync) and convert here, outside every guard.
bool VmExecutor::ExecuteIntrinsicSync(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    try
    {
        switch (intrinsicId)
        {
        case INTR_Mutex_Ctor:
            return IntrinsicMutexCtor(callParamBase, locals);
        case INTR_Mutex_Lock:
            return IntrinsicMutexLock(callParamBase, locals);
        case INTR_Mutex_Unlock:
            return IntrinsicMutexUnlock(callParamBase, locals);
        case INTR_Mutex_TryLock:
            return IntrinsicMutexTryLock(callParamBase, locals, pResult);
        case INTR_Channel_Ctor:
            return IntrinsicChannelCtor(callParamBase, locals);
        case INTR_Channel_Send:
            return IntrinsicChannelSend(callParamBase, locals);
        case INTR_Channel_Recv:
            return IntrinsicChannelRecv(callParamBase, locals, pResult);
        case INTR_Channel_Close:
            return IntrinsicChannelClose(callParamBase, locals);
        case INTR_Semaphore_Ctor:
            return IntrinsicSemaphoreCtor(callParamBase, locals);
        case INTR_Semaphore_Acquire:
            return IntrinsicSemaphoreAcquire(callParamBase, locals);
        case INTR_Semaphore_Release:
            return IntrinsicSemaphoreRelease(callParamBase, locals);
        case INTR_Semaphore_TryAcquire:
            return IntrinsicSemaphoreTryAcquire(callParamBase, locals,
                                                pResult);
        default:
            return false;
        }
    }
    catch (DeferredNlangRaise& r)
    {
        RaiseNlangException(r.classIdx, r.message);   //[[noreturn]]
    }
}

//thread.spawn(f) -> Thread | Future<T>. The root frame is built AT spawn
//(plan decision 6): the delegate's target resolves now (virtual handles
//bind the receiver's runtime class), the receiver rides slot 0 for bound
//handles, and the whole frame parks in the new context — the precise
//frame-local scan covers it from the first GC, no extra root storage.
//K3: the delegate's return kind decides the handle face — void keeps
//the K1 Thread object, anything else mints a Future<T>; resultKind
//snapshots the same kind for the GC's finished-context root arm.
bool VmExecutor::IntrinsicSpawn(uint16_t callParamBase, uint8_t* locals,
                                uint8_t* pResult)
{
    //K1 Slice 10: without the carrier in flight nothing ever schedules
    //the new context. Two nested-window shapes differ: a host callback
    //taken mid-run (carrier active — the loop picks the context up after
    //the callback returns) and the embed call() window (no carrier —
    //spawn would silently mint a never-run context whose Thread object
    //looks alive while join always rejects). Refuse the second at the
    //spawn site.
    if (m_spScheduler->ActiveCarrierCount() == 0)
        throw std::runtime_error(
            "NLang VM: thread.spawn requires an active scheduler carrier "
            "(a host call() window cannot schedule the new context)");
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
    //K3: the GC root arm classifies a Finished context's resultCell by
    //this kind — a reference-T Future would lose its value before get().
    ctx->resultKind = callee.returnTypeKind;
    CallFrame root;
    root.localsBuf.assign(callee.localsSize, 0);
    if (receiverIdx != 0)
        std::memcpy(root.localsBuf.data(), &receiverIdx,
                    sizeof(receiverIdx));
    //Result threads to the context's own cell (deque-stable across later
    //registrations — see VmScheduler.h). Writes are ≤ kFrameSlotBytes
    //memcpys, so the int64_t cell is a valid uniform result cell.
    root.externalResult = reinterpret_cast<uint8_t*>(&ctx->resultCell);
    root.func = &callee;
    ctx->frames.push_back(std::move(root));
    int32_t handleObj = (callee.returnTypeKind != RTK_Void)
        ? MintFutureObject(ctxId) : MintThreadObject(ctxId);
    ctx->threadObjHeapIdx = handleObj;
    std::memcpy(pResult, &handleObj, sizeof(handleObj));
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
            "NLang VM: thread.spawn requires a zero-argument function");
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
    if (target->state == ContextState::Failed) {
        m_spScheduler->MarkFailureObserved(targetId);
        ThrowJoinTargetFailure(target->failExcHeapIdx);
    }
    //Gate BEFORE mutating scheduler state (decision 2 spec (a)): the
    //nested-window rejection must not leave a half-parked context.
    EnsureSuspensionAllowed();
    m_spScheduler->ParkForJoin(selfId, targetId);
    RequestPark(ParkSignal::Reason::Join);   //[[noreturn]]
}

//thread.sleep(ms)。steady_clock 截止时刻经 ParkForTimer 登记（状态 +
//定时器表），RequestPark 抛出信号、载体偷帧；恢复点由 OP_CallFunc 内建
//函数处理分支的 pc 推进不变量保证（同 yield/join）。sleep(0) 是纯让出：
//立即到期的定时器使载体先运行队列中的其他上下文。负时长是对语言面的
//误用——基础异常拒绝（同 spawn 目标拒绝形态）。
bool VmExecutor::IntrinsicSleep(uint16_t callParamBase, uint8_t* locals)
{
    //Argument 0: the millisecond count (int).
    int32_t ms;
    std::memcpy(&ms, locals + callParamBase, sizeof(ms));
    if (ms < 0)
        throw std::runtime_error(
            "NLang VM: thread.sleep requires a non-negative duration");
    //Gate BEFORE mutating scheduler state (decision 2 spec (a)): the
    //nested-window rejection must not leave a half-parked context.
    EnsureSuspensionAllowed();
    const auto wakeAt = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(ms);
    m_spScheduler->ParkForTimer(m_spScheduler->CurrentContextId(), wakeAt);
    RequestPark(ParkSignal::Reason::Timer);   //[[noreturn]]
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
