/*---
    VmScheduler.cpp — 单载体调度器实现（并发 K1）
    Slice 4 骨架 + Slice 6 join 停泊/唤醒 + Slice 7 定时器（空队列时
    载体睡至最近到期）+ Slice 8 互斥停泊/唤醒。
---*/
#include "VmScheduler.h"
#include <thread>

namespace nlang {

void VmScheduler::Reset() {
    m_contexts.clear();
    m_runQueue.clear();
    m_timers.clear();
    m_currentId = -1;
    m_nextId = 0;
}

int VmScheduler::RegisterContext() {
    const int id = m_nextId++;
    ExecutionContext ctx;
    ctx.id = id;
    m_contexts.push_back(std::move(ctx));
    m_runQueue.push_back(id);
    return id;
}

//空队列时的定时器处理分支：睡至最近到期（睡眠中的上下文不可运行，载体闲
//等是唯一选择——单载体无他人可调度），迁移到期者后回到正常出队。全
//部停泊均无定时器时本轮运行结束（-1）。循环而非单次睡眠：防御到期
//条目全部陈旧的退化形态（K1 构造不出，见 ExpireDueTimers）。
int VmScheduler::PopNextRunnable() {
    for (;;) {
        if (!m_runQueue.empty())
            break;
        if (m_timers.empty())
            return -1;
        auto nearest = m_timers.front().wakeAt;
        for (const TimerEntry& t : m_timers)
            if (t.wakeAt < nearest)
                nearest = t.wakeAt;
        std::this_thread::sleep_until(nearest);
        ExpireDueTimers();
    }
    const int id = m_runQueue.front();
    m_runQueue.pop_front();
    m_currentId = id;
    return id;
}

void VmScheduler::Requeue(int id) {
    m_runQueue.push_back(id);
    if (m_currentId == id)
        m_currentId = -1;
}

void VmScheduler::MarkFinished(int id, int64_t result) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Finished;
    ctx->resultCell = result;
    if (m_currentId == id)
        m_currentId = -1;
}

void VmScheduler::MarkFailed(int id, int32_t failExcHeapIdx) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Failed;
    ctx->failExcHeapIdx = failExcHeapIdx;
    if (m_currentId == id)
        m_currentId = -1;
}

//join 停泊（Slice 6）：状态与目标登记；调用方（载体）随后把帧整栈搬
//入 ctx->frames。不入运行队列——只有目标终结才唤醒。
void VmScheduler::ParkForJoin(int id, int targetId) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedJoin;
    ctx->parkHandle = targetId;
    if (m_currentId == id)
        m_currentId = -1;
}

//目标终结：全部 ParkedJoin 于该目标的上下文回 Runnable 入队尾；
//failed 目标同时置 rethrowOnResume——joiner 恢复执行前重抛目标的
//原异常对象（保留 raise 时的回溯）。
void VmScheduler::WakeJoiners(int targetId, bool failed) {
    for (auto& ctx : m_contexts) {
        if (ctx.state != ContextState::ParkedJoin
            || ctx.parkHandle != targetId)
            continue;
        ctx.state = ContextState::Runnable;
        if (failed)
            ctx.rethrowOnResume = true;
        m_runQueue.push_back(ctx.id);
    }
}

//sleep 停泊（Slice 7）：状态与唤醒时刻登记、入定时器表；当前上下文
//让出载体。帧的搬运由载体在 ParkSignal 捕获处统一完成。
void VmScheduler::ParkForTimer(int id,
                               std::chrono::steady_clock::time_point wakeAt) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedTimer;
    ctx->wakeAt = wakeAt;
    m_timers.push_back(TimerEntry{id, wakeAt});
    if (m_currentId == id)
        m_currentId = -1;
}

//到期迁移（见头文件）。线性表规模 = 停泊中的 sleep 上下文数，K1 量级
//下无需堆结构。陈旧条目（状态已非 ParkedTimer）必须丢弃：保留会使
//PopNextRunnable 对一个已过的截止时间无限空转（活锁）。
void VmScheduler::ExpireDueTimers() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<TimerEntry> remaining;
    remaining.reserve(m_timers.size());
    for (const TimerEntry& t : m_timers) {
        ExecutionContext* ctx = FindMutable(t.contextId);
        if (!ctx || ctx->state != ContextState::ParkedTimer)
            continue;   //stale — dropped, never re-armed
        if (t.wakeAt <= now) {
            ctx->state = ContextState::Runnable;
            m_runQueue.push_back(ctx->id);
        } else {
            remaining.push_back(t);
        }
    }
    m_timers = std::move(remaining);
}

//互斥停泊（Slice 8）：仅状态迁移——等待者集合在执行器的 MutexSlot 侧
//表（FIFO），唤醒由 unlock 的所有权移交显式触发（WakeForMutex）。
void VmScheduler::ParkForMutex(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedMutex;
    if (m_currentId == id)
        m_currentId = -1;
}

//互斥唤醒（Slice 8）：unlock 把所有权移交给队首等待者后调用——回
//Runnable 入队尾。
void VmScheduler::WakeForMutex(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Runnable;
    m_runQueue.push_back(id);
}

//通道接收停泊（Slice 9）：仅状态迁移——等待者集合（含各等待者的结果
//写回指针）在执行器的 ChannelSlot 侧表，唤醒由 send 的直接交付或
//close 的全员唤醒显式触发（WakeForChanRecv）。
void VmScheduler::ParkForChanRecv(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedChanRecv;
    if (m_currentId == id)
        m_currentId = -1;
}

//通道唤醒（Slice 9）：回 Runnable 入队尾。值已由执行器在调用本方法
//之前写入停泊帧的结果槽（停泊的调用不会重执行——pc 推进不变量）。
void VmScheduler::WakeForChanRecv(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Runnable;
    m_runQueue.push_back(id);
}

const ExecutionContext* VmScheduler::Find(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= m_contexts.size())
        return nullptr;
    return &m_contexts[static_cast<size_t>(id)];
}

ExecutionContext* VmScheduler::FindMutable(int id) {
    return const_cast<ExecutionContext*>(Find(id));
}

} // namespace nlang
