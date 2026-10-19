/*---
    IntrinsicsThread.cpp — K1 并发内在函数族：
      · thread.* 自由调用（129-133）：经 OP_CallFunc 的内建函数分支
        派发，参数自 callParamBase 槽 0 起（无 this）；
      · Mutex/Channel 方法（134-141）：接收者派发形态（槽 0 = this）。
    Slice 5 接入 yield/id，Slice 6 接入 spawn/join，Slice 7 接入 sleep，
    Slice 8 接入 Mutex 全族，Slice 9 接入 Channel 全族——K1 内在函数族
    至此全部落地。
---*/
#include "VmExecutor.h"
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace nlang {

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
    case INTR_Thread_Sleep:
        return IntrinsicSleep(callParamBase, locals);
    //Slice 8: the Mutex family (see the bodies below the join/sleep
    //intrinsics — receiver-dispatch shape, slot 0 = the instance).
    case INTR_Mutex_Ctor:
        return IntrinsicMutexCtor(callParamBase, locals);
    case INTR_Mutex_Lock:
        return IntrinsicMutexLock(callParamBase, locals);
    case INTR_Mutex_Unlock:
        return IntrinsicMutexUnlock(callParamBase, locals);
    case INTR_Mutex_TryLock:
        return IntrinsicMutexTryLock(callParamBase, locals, pResult);
    //Slice 9: the Channel family (see the bodies below the Mutex
    //helpers — receiver-dispatched shape, slot 0 = the instance).
    case INTR_Channel_Ctor:
        return IntrinsicChannelCtor(callParamBase, locals);
    case INTR_Channel_Send:
        return IntrinsicChannelSend(callParamBase, locals);
    case INTR_Channel_Recv:
        return IntrinsicChannelRecv(callParamBase, locals, pResult);
    case INTR_Channel_Close:
        return IntrinsicChannelClose(callParamBase, locals);
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

//Slice 8: Mutex() 构造——物化侧表槽位并把句柄写进 __handle（同 List
//构造形态：接收者即 callParamBase 槽 0 的实例记录）。
bool VmExecutor::IntrinsicMutexCtor(uint16_t callParamBase, uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in Mutex constructor");
    m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset]
        = AllocMutexHandle();
    return true;
}

//lock()：空闲即取（ownerId 记当前上下文）；被持（含自持——不可重入）
//则 FIFO 停泊，unlock 移交。停泊经 ParkSignal(Mutex)；恢复点由调用
//派发分支的 pc 推进不变量保证（同 yield/join/sleep）。
bool VmExecutor::IntrinsicMutexLock(uint16_t callParamBase, uint8_t* locals)
{
    MutexSlot& slot = ResolveMutexSlot(callParamBase, locals);
    const int selfId = m_spScheduler->CurrentContextId();
    if (slot.ownerId < 0) {
        slot.ownerId = selfId;
        return true;
    }
    //Gate BEFORE mutating scheduler state (decision 2 spec (a)): the
    //nested-window rejection must not leave a half-parked context.
    EnsureSuspensionAllowed();
    slot.waiters.push_back(selfId);
    m_spScheduler->ParkForMutex(selfId);
    RequestPark(ParkSignal::Reason::Mutex);   //[[noreturn]]
}

//tryLock()：空闲取之返 true，被持返 false——绝不停泊（唯一不挂起的探
//测路径，可在嵌套求值窗口内安全调用）。
bool VmExecutor::IntrinsicMutexTryLock(uint16_t callParamBase,
                                       uint8_t* locals, uint8_t* pResult)
{
    MutexSlot& slot = ResolveMutexSlot(callParamBase, locals);
    const bool acquired = slot.ownerId < 0;
    if (acquired)
        slot.ownerId = m_spScheduler->CurrentContextId();
    int32_t r = acquired ? 1 : 0;
    std::memcpy(pResult, &r, sizeof(r));
    return true;
}

//unlock()：未持有 → 基础异常；有等待者 → 所有权移交队首并唤醒（FIFO，
//移交与唤醒原子——不存在锁看似空闲的窗口）；无等待者 → 置空闲。
bool VmExecutor::IntrinsicMutexUnlock(uint16_t callParamBase, uint8_t* locals)
{
    MutexSlot& slot = ResolveMutexSlot(callParamBase, locals);
    const int selfId = m_spScheduler->CurrentContextId();
    if (slot.ownerId != selfId)
        throw std::runtime_error(
            "NLang VM: Mutex.unlock without holding the lock");
    if (slot.waiters.empty()) {
        slot.ownerId = -1;
        return true;
    }
    const int next = slot.waiters.front();
    slot.waiters.pop_front();
    slot.ownerId = next;
    m_spScheduler->WakeForMutex(next);
    return true;
}

//Slice 8 共享：解析接收者实例（callParamBase 槽 0）到侧表槽位。句柄即
//__handle 字段（kListHandleFieldOffset，List/Dict 同布局）。
VmExecutor::MutexSlot& VmExecutor::ResolveMutexSlot(
    uint16_t callParamBase, uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in Mutex call");
    const int32_t handle =
        m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset];
    if (handle <= 0 || static_cast<size_t>(handle) > m_mutexStore.size())
        throw std::runtime_error(
            "NLang VM: Mutex call on an uninitialized instance");
    return m_mutexStore[static_cast<size_t>(handle) - 1];
}

//Slice 8：分配互斥体侧表槽位（句柄 = 下标+1，0 保留给 null）。复用槽
//位时整体重置——清扫回收只归还句柄，owner/waiters 可能残留被丢弃锁
//（持锁时实例死亡的程序缺陷形态）的状态。
int32_t VmExecutor::AllocMutexHandle() {
    if (!m_mutexFreeList.empty()) {
        const int32_t handle = m_mutexFreeList.back();
        m_mutexFreeList.pop_back();
        m_mutexStore[static_cast<size_t>(handle) - 1] = MutexSlot();
        return handle;
    }
    m_mutexStore.emplace_back();
    return static_cast<int32_t>(m_mutexStore.size());
}

//Slice 9: Channel() 构造——物化侧表槽位并把句柄写进 __handle（同
//Mutex 构造形态：接收者即 callParamBase 槽 0 的实例记录）。
bool VmExecutor::IntrinsicChannelCtor(uint16_t callParamBase, uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in Channel constructor");
    m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset]
        = AllocChannelHandle();
    return true;
}

//send(v)：永不停泊（无界缓冲）。closed → 基础异常；有等待者 → 值直接
//交付队首（缓冲必空——不变量）；否则入缓冲尾。值即擦除后的堆句柄（装箱
//记录或类引用）。
bool VmExecutor::IntrinsicChannelSend(uint16_t callParamBase, uint8_t* locals)
{
    ChannelSlot& slot = ResolveChannelSlot(callParamBase, locals);
    if (slot.closed)
        throw std::runtime_error("NLang VM: send on closed channel");
    int32_t value;
    std::memcpy(&value, locals + callParamBase + kFrameSlotBytes,
                sizeof(value));
    if (!slot.waiters.empty()) {
        DeliverToWaiter(slot.waiters.front(), value);
        slot.waiters.pop_front();
        return true;
    }
    slot.buffered.push_back(value);
    return true;
}

//recv()：缓冲非空 → 弹头写 pResult；closed 且空 → 写 0（null 引用；T
//为基元类型时由调用点拆箱报错——擦除机制在运行期无从合成带 T 标签的
//零值装箱记录，且基元类型不可空，无值即诚实结局）；空且开放 → 停泊：
//登记唤醒时写值的目标指针 pResult（停泊的调用不会重执行——pc 推进不
//变量），send 的直接交付或 close 的全员唤醒经 DeliverToWaiter 写回。
bool VmExecutor::IntrinsicChannelRecv(uint16_t callParamBase, uint8_t* locals,
                                      uint8_t* pResult)
{
    ChannelSlot& slot = ResolveChannelSlot(callParamBase, locals);
    if (!slot.buffered.empty()) {
        const int32_t value = slot.buffered.front();
        slot.buffered.pop_front();
        std::memcpy(pResult, &value, sizeof(value));
        return true;
    }
    if (slot.closed) {
        const int32_t nullValue = 0;
        std::memcpy(pResult, &nullValue, sizeof(nullValue));
        return true;
    }
    //Gate BEFORE mutating scheduler state (decision 2 spec (a)): the
    //nested-window rejection must not leave a half-parked context.
    EnsureSuspensionAllowed();
    const int selfId = m_spScheduler->CurrentContextId();
    slot.waiters.push_back(ChannelWaiter{selfId, pResult});
    m_spScheduler->ParkForChanRecv(selfId);
    RequestPark(ParkSignal::Reason::ChanRecv);   //[[noreturn]]
}

//close()：幂等置位；首闭时唤醒全部等待者——缓冲值先排干再交 0（Go 语
//义：已发送的值先于 null 被取走）。等待者必伴随空缓冲（不变量），排干
//分支为防御形态。
bool VmExecutor::IntrinsicChannelClose(uint16_t callParamBase, uint8_t* locals)
{
    ChannelSlot& slot = ResolveChannelSlot(callParamBase, locals);
    if (slot.closed)
        return true;
    slot.closed = true;
    while (!slot.waiters.empty()) {
        int32_t value = 0;
        if (!slot.buffered.empty()) {
            value = slot.buffered.front();
            slot.buffered.pop_front();
        }
        DeliverToWaiter(slot.waiters.front(), value);
        slot.waiters.pop_front();
    }
    return true;
}

//Slice 9 共享：解析接收者实例（callParamBase 槽 0）到侧表槽位（同
//Mutex 形态）。
VmExecutor::ChannelSlot& VmExecutor::ResolveChannelSlot(
    uint16_t callParamBase, uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in Channel call");
    const int32_t handle =
        m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset];
    if (handle <= 0 || static_cast<size_t>(handle) > m_channelStore.size())
        throw std::runtime_error(
            "NLang VM: Channel call on an uninitialized instance");
    return m_channelStore[static_cast<size_t>(handle) - 1];
}

//Slice 9：分配通道侧表槽位（句柄 = 下标+1，0 保留给 null）。复用槽位
//时整体重置——清扫回收只归还句柄，缓冲/等待者可能残留被丢弃通道的状
//态（与互斥体同纪律）。
int32_t VmExecutor::AllocChannelHandle() {
    if (!m_channelFreeList.empty()) {
        const int32_t handle = m_channelFreeList.back();
        m_channelFreeList.pop_back();
        m_channelStore[static_cast<size_t>(handle) - 1] = ChannelSlot();
        return handle;
    }
    m_channelStore.emplace_back();
    return static_cast<int32_t>(m_channelStore.size());
}

//唤醒时交付（send 直接交付与 close 全员唤醒共用）：先写值进停泊帧的
//结果槽、再回 Runnable——被唤醒的上下文经载体出队执行，读到已写好的
//返回值。
void VmExecutor::DeliverToWaiter(ChannelWaiter& waiter, int32_t value)
{
    std::memcpy(waiter.resultPtr, &value, sizeof(value));
    m_spScheduler->WakeForChanRecv(waiter.contextId);
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
