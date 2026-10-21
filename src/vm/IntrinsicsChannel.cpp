/*---
    IntrinsicsChannel.cpp — K1 Slice 9 / K3 Task 4 Channel<T> 内在函数
    族（138-141）：构造（容量契约）、send/recv/close、侧表槽位与等待者
    交付。从 IntrinsicsThread.cpp 抽出（源文件 500 行预算）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//Slice 9: Channel() 构造——物化侧表槽位并把句柄写进 __handle（同
//Mutex 构造形态：接收者即 callParamBase 槽 0 的实例记录）。K3 Task 4:
//槽 1 = 容量（两个发射点都显式摆值；0 = 无界），负值抛可捕获的容量错误。
bool VmExecutor::IntrinsicChannelCtor(uint16_t callParamBase, uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in Channel constructor");
    int32_t capacity;
    std::memcpy(&capacity, locals + callParamBase + kFrameSlotBytes,
                sizeof(capacity));
    if (capacity < kChannelUnboundedCapacity)
        RaiseNlangException(m_exceptionClassIdx,
            "channel capacity must be 0 (unbounded) or at least 1");
    const int32_t handle = AllocChannelHandle();
    m_channelStore[static_cast<size_t>(handle) - 1].capacity = capacity;
    m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset]
        = handle;
    return true;
}

//send(v)：无界缓冲时永不停泊（K3 Task 4 起有界容量可停泊）。closed →
//抛可捕获的 ChannelClosedException（语言面异常，用户代码可恢复）；
//有等待者 → 值直接交付队首（缓冲必空——不变量）；否则入缓冲尾。值即
//擦除后的堆句柄（装箱记录或类引用）。
bool VmExecutor::IntrinsicChannelSend(uint16_t callParamBase, uint8_t* locals)
{
    ChannelSlot& slot = ResolveChannelSlot(callParamBase, locals);
    if (slot.closed)
        RaiseNlangException(m_channelClosedExcClassIdx,
                            "send on a closed channel");
    int32_t value;
    std::memcpy(&value, locals + callParamBase + kFrameSlotBytes,
                sizeof(value));
    if (!slot.waiters.empty()) {
        DeliverToWaiter(slot.waiters.front(), value);
        slot.waiters.pop_front();
        return true;
    }
    //K3 Task 4: bounded and full (a waiter implies an empty buffer, so
    //reaching here means the buffer owns every slot) → park value+owner
    //FIFO; recv's slot-free move wakes the owner with the value already
    //staged into the buffer.
    if (slot.capacity > 0
        && slot.buffered.size() >= static_cast<size_t>(slot.capacity)) {
        EnsureSuspensionAllowed();
        const int selfId = m_spScheduler->CurrentContextId();
        slot.pendingSends.push_back(value);
        slot.pendingSenders.push_back(selfId);
        m_spScheduler->ParkForChanSend(selfId);
        RequestPark(ParkSignal::Reason::ChanSend);   //[[noreturn]]
    }
    slot.buffered.push_back(value);
    return true;
}

//recv()：缓冲非空 → 弹头写 pResult；closed 且空 → 抛可捕获的
//ChannelClosedException（K3 Task 3：诚实的通道终点，取代 K1 的 null
//返回——拆箱形态下 null 对基元 T 本就无解）；空且开放 → 停泊：登记唤
//醒时写值的目标指针 pResult（停泊的调用不会重执行——pc 推进不变量），
//send 的直接交付或 close 的交付/异常唤醒写回。
bool VmExecutor::IntrinsicChannelRecv(uint16_t callParamBase, uint8_t* locals,
                                      uint8_t* pResult)
{
    ChannelSlot& slot = ResolveChannelSlot(callParamBase, locals);
    if (!slot.buffered.empty()) {
        const int32_t value = slot.buffered.front();
        slot.buffered.pop_front();
        //K3 Task 4: a slot freed — stage the head pending value into the
        //buffer and wake its owner (resume only; the parked call is not
        //re-executed, its value is already queued).
        if (!slot.pendingSenders.empty()) {
            slot.buffered.push_back(slot.pendingSends.front());
            slot.pendingSends.pop_front();
            m_spScheduler->WakeForChanSend(slot.pendingSenders.front());
            slot.pendingSenders.pop_front();
        }
        std::memcpy(pResult, &value, sizeof(value));
        return true;
    }
    if (slot.closed)
        RaiseNlangException(m_channelClosedExcClassIdx,
                            "recv on a closed and drained channel");
    //Gate BEFORE mutating scheduler state (decision 2 spec (a)): the
    //nested-window rejection must not leave a half-parked context.
    EnsureSuspensionAllowed();
    const int selfId = m_spScheduler->CurrentContextId();
    slot.waiters.push_back(ChannelWaiter{selfId, pResult});
    m_spScheduler->ParkForChanRecv(selfId);
    RequestPark(ParkSignal::Reason::ChanRecv);   //[[noreturn]]
}

//close()：幂等置位；首闭时唤醒全部等待者——缓冲值先排干（已发送的值
//先于关闭信号被取走），排干后剩余等待者以 ChannelClosedException 唤醒
//（K3 Task 3：停泊中的消费者同样看见诚实的通道终点）。等待者必伴随空
//缓冲（不变量），异常分支为常规形态。
bool VmExecutor::IntrinsicChannelClose(uint16_t callParamBase, uint8_t* locals)
{
    ChannelSlot& slot = ResolveChannelSlot(callParamBase, locals);
    if (slot.closed)
        return true;
    slot.closed = true;
    //K3 Task 3: waiters the buffer can't satisfy wake with the
    //catchable ChannelClosedException instead of a silent null. The
    //object is prebuilt once (first drained waiter) and rooted the
    //moment it lands on the waiter's context — a scheduler root — so
    //the gap until that context resumes is covered. No GC safepoint
    //exists inside this intrinsic, so the build-and-arm window is
    //atomic with respect to collection.
    int32_t closedExc = 0;
    while (!slot.waiters.empty()) {
        ChannelWaiter waiter = slot.waiters.front();
        slot.waiters.pop_front();
        if (!slot.buffered.empty()) {
            const int32_t value = slot.buffered.front();
            slot.buffered.pop_front();
            DeliverToWaiter(waiter, value);
            continue;
        }
        ExecutionContext* parkedCtx
            = m_spScheduler->FindMutable(waiter.contextId);
        if (!parkedCtx)
            continue;   //corrupt registry — nothing to wake
        if (closedExc == 0)
            closedExc = BuildNlangException(m_channelClosedExcClassIdx,
                "recv on a closed and drained channel");
        parkedCtx->rethrowOnResume = true;
        parkedCtx->resumeExcHeapIdx = closedExc;
        m_spScheduler->WakeForChanRecv(waiter.contextId);
    }
    //K3 Task 4: parked senders wake with the same catchable exception;
    //their pending values are dropped (never delivered — the channel is
    //closed).
    WakeClosedPendingSenders(slot);
    return true;
}

//K3 Task 4: close 的发送者臂——每个停泊发送者以可捕获的 closed 异常唤
//醒（共享一个预建对象并随赋值即扎根于调度器根，同 recv 臂纪律）。
void VmExecutor::WakeClosedPendingSenders(ChannelSlot& slot)
{
    int32_t closedExc = 0;
    while (!slot.pendingSenders.empty()) {
        const int senderId = slot.pendingSenders.front();
        slot.pendingSenders.pop_front();
        slot.pendingSends.pop_front();
        ExecutionContext* parkedCtx = m_spScheduler->FindMutable(senderId);
        if (!parkedCtx)
            continue;   //corrupt registry — nothing to wake
        if (closedExc == 0)
            closedExc = BuildNlangException(m_channelClosedExcClassIdx,
                "send on a closed channel");
        parkedCtx->rethrowOnResume = true;
        parkedCtx->resumeExcHeapIdx = closedExc;
        m_spScheduler->WakeForChanSend(senderId);
    }
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
} // namespace nlang
