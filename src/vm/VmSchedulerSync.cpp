/*---
    VmSchedulerSync.cpp — K8: the Sync-primitive park/wake family
    (mutex handoff, channel recv/send parking, the two-phase handshake
    abort). Split out of VmScheduler.cpp (the 500-line source budget);
    the state migrations all run under the scheduler domain lock.
---*/
#include "VmScheduler.h"

namespace nlang {

//互斥停泊（Slice 8）：仅状态迁移——等待者集合在执行器的 MutexSlot 侧
//表（FIFO），唤醒由 unlock 的所有权移交显式触发（WakeForMutex）。
void VmScheduler::ParkForMutex(int id) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedMutex;
    ReleaseHeldIfCurrent(id);
    //K4：焦点停泊——门等待者放行。
    NotifyGateWaiters();
}

//互斥唤醒（Slice 8）：unlock 把所有权移交给队首等待者后调用——回
//Runnable；已完成停泊者入队尾（K7 握手，见
//ExecutionContext::framesParked）。
void VmScheduler::WakeForMutex(int id) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Runnable;
    if (ctx->framesParked)
        m_runQueue.push_back(id);
    m_runQueueCv.notify_all();
}

//通道接收停泊（Slice 9）：仅状态迁移——等待者集合（含各等待者的结果
//写回指针）在执行器的 ChannelSlot 侧表，唤醒由 send 的直接交付或
//close 的全员唤醒显式触发（WakeForChanRecv）。
void VmScheduler::ParkForChanRecv(int id) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedChanRecv;
    ReleaseHeldIfCurrent(id);
    //K4：焦点停泊——门等待者放行。
    NotifyGateWaiters();
}

//通道唤醒（Slice 9）：回 Runnable；已完成停泊者入队尾（K7 握手，见
//ExecutionContext::framesParked）。值已由执行器在调用本方法之前写入
//停泊帧的结果槽（停泊的调用不会重执行——pc 推进不变量）。K8：close
//的预建异常在同一临界区内置位（恢复路径或中止位点消费）。
void VmScheduler::WakeForChanRecv(int id, int32_t rethrowExcHeapIdx) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    if (rethrowExcHeapIdx > 0) {
        ctx->rethrowOnResume = true;
        ctx->resumeExcHeapIdx = rethrowExcHeapIdx;
    }
    ctx->state = ContextState::Runnable;
    if (ctx->framesParked)
        m_runQueue.push_back(id);
    m_runQueueCv.notify_all();
}

//K3 task 4：有界通道发送停泊——仅状态迁移，待发值/属主队列在执行器的
//ChannelSlot 侧表。
void VmScheduler::ParkForChanSend(int id) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedChanSend;
    ReleaseHeldIfCurrent(id);
    //K4：焦点停泊——门等待者放行。
    NotifyGateWaiters();
}

//K3 task 4：发送者唤醒——回 Runnable；已完成停泊者入队尾（K7 握手，
//见 ExecutionContext::framesParked）。recv 已把队首待发值挪入缓冲
//（或 close 已预置异常重抛，K8 同 WakeForChanRecv 的句柄参数），
//停泊的调用不会重执行。
void VmScheduler::WakeForChanSend(int id, int32_t rethrowExcHeapIdx) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    if (rethrowExcHeapIdx > 0) {
        ctx->rethrowOnResume = true;
        ctx->resumeExcHeapIdx = rethrowExcHeapIdx;
    }
    ctx->state = ContextState::Runnable;
    if (ctx->framesParked)
        m_runQueue.push_back(id);
    m_runQueueCv.notify_all();
}

//K8：见头文件。只恢复 ParkFor* 变异过的两样（state＋持有镜像）——故
//意不走 DispatchedToCarrier（那会多记一次辅助分派；中止是同一次分派
//的延续）。
void VmScheduler::AbortPark(int id) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Runnable;
    if (!t_carrier)
        return;
    t_carrier->currentContextId = id;
    if (t_carrier->carrierId >= 0
        && static_cast<size_t>(t_carrier->carrierId)
               < m_heldByCarrier.size())
        m_heldByCarrier[static_cast<size_t>(t_carrier->carrierId)] = id;
}

} // namespace nlang
