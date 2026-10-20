/*---
    VmScheduler.cpp — 上下文调度器实现（并发 K1；K2 多载体化 2026-10-20）
    Slice 4 骨架 + Slice 6 join 停泊/唤醒 + Slice 7 定时器 + Slice 8 互斥
    停泊/唤醒 + K2：条件变量谓词等待、关停通知、死锁检测、每载体状态。
---*/
#include "VmScheduler.h"
#include <thread>

namespace nlang {

thread_local VmScheduler::CarrierState VmScheduler::t_selfCarrier;
thread_local VmScheduler::CarrierState* VmScheduler::t_carrier = nullptr;

void VmScheduler::Reset() {
    m_contexts.clear();
    m_runQueue.clear();
    m_timers.clear();
    m_stopRequested = false;
    m_idleCarrierCount = 0;
    m_nextId = 0;
    m_auxDispatchCount = 0;
    m_gilWaitersPeak.store(0, std::memory_order_relaxed);
}

int VmScheduler::RegisterContext() {
    //RegisterContext is the primary carrier's exclusive step (it runs
    //before the pool launches) — the calling thread is therefore the
    //primary carrier thread; record it for the aux-dispatch counter.
    m_primaryThreadId = std::this_thread::get_id();
    const int id = m_nextId++;
    ExecutionContext ctx;
    ctx.id = id;
    m_contexts.push_back(std::move(ctx));
    m_runQueue.push_back(id);
    m_runQueueCv.notify_all();
    return id;
}

//空队列时的等待分支（须持 GIL，gil 即其锁——wait 期间释放、醒来重
//取）：全部载体在条件变量上谓词等待，任何入队/关停路径 notify_all 唤
//醒。若仍有定时器，等待界限取最近到期时刻，醒来后迁移到期者再回到
//正常出队（循环而非单次等待：一次醒来可能只迁移定时器、或到期条目
//全部陈旧）。死锁检测在进入等待前：全部在飞载体都将等待（本载体即
//将成为最后一个空闲者）且队列与定时器表皆空——没有任何唤醒源，直
//接抛错；main 未完成即无法返回，与其静默挂死不如拒绝。等待中的载体
//计入 m_idleCarrierCount（锁内维护，检测的输入）。
int VmScheduler::PopNextRunnable(std::unique_lock<std::mutex>& gil) {
    for (;;) {
        if (!m_runQueue.empty()) {
            const int id = m_runQueue.front();
            m_runQueue.pop_front();
            t_carrier->currentContextId = id;
            if (std::this_thread::get_id() != m_primaryThreadId)
                ++m_auxDispatchCount;
            return id;
        }
        if (m_stopRequested)
            return -1;
        if (m_timers.empty()
                && m_idleCarrierCount + 1 >= m_activeCarrierCount.load()) {
            throw std::runtime_error(
                "NLang VM: deadlock — no runnable contexts remain");
        }
        ++m_idleCarrierCount;
        if (m_timers.empty()) {
            m_runQueueCv.wait(gil, [this] {
                return m_stopRequested || !m_runQueue.empty();
            });
        } else {
            auto nearest = m_timers.front().wakeAt;
            for (const TimerEntry& t : m_timers)
                if (t.wakeAt < nearest)
                    nearest = t.wakeAt;
            m_runQueueCv.wait_until(gil, nearest, [this] {
                return m_stopRequested || !m_runQueue.empty();
            });
            ExpireDueTimers();
        }
        --m_idleCarrierCount;
    }
}

void VmScheduler::Requeue(int id) {
    m_runQueue.push_back(id);
    m_runQueueCv.notify_all();
    if (t_carrier && t_carrier->currentContextId == id)
        t_carrier->currentContextId = -1;
}

void VmScheduler::MarkFinished(int id, int64_t result) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Finished;
    ctx->resultCell = result;
    if (t_carrier && t_carrier->currentContextId == id)
        t_carrier->currentContextId = -1;
    //main 终结即本轮运行结束：请求关停，等待中的辅助载体在下次出队
    //看到 -1 退出（挂起上下文静默丢弃——K1 退出策略不变）。
    if (id == kMainThreadId) {
        m_stopRequested = true;
        m_runQueueCv.notify_all();
    }
}

void VmScheduler::MarkFailed(int id, int32_t failExcHeapIdx) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Failed;
    ctx->failExcHeapIdx = failExcHeapIdx;
    if (t_carrier && t_carrier->currentContextId == id)
        t_carrier->currentContextId = -1;
    //main 失败同样终结本轮运行（错误经主载体重抛到宿主）。
    if (id == kMainThreadId) {
        m_stopRequested = true;
        m_runQueueCv.notify_all();
    }
}

//join 停泊（Slice 6）：状态与目标登记；调用方（载体）随后把帧整栈搬
//入 ctx->frames。不入运行队列——只有目标终结才唤醒。
void VmScheduler::ParkForJoin(int id, int targetId) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedJoin;
    ctx->parkHandle = targetId;
    if (t_carrier && t_carrier->currentContextId == id)
        t_carrier->currentContextId = -1;
}

//目标终结：全部 ParkedJoin 于该目标的上下文回 Runnable 入队尾；
//failed 目标同时置 rethrowOnResume——joiner 恢复执行前重抛目标的
//原异常对象（保留 raise 时的回溯）。多 joiner 皆唤醒（计划决策 6）。
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
    m_runQueueCv.notify_all();
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
    if (t_carrier && t_carrier->currentContextId == id)
        t_carrier->currentContextId = -1;
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
    if (t_carrier && t_carrier->currentContextId == id)
        t_carrier->currentContextId = -1;
}

//互斥唤醒（Slice 8）：unlock 把所有权移交给队首等待者后调用——回
//Runnable 入队尾。
void VmScheduler::WakeForMutex(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Runnable;
    m_runQueue.push_back(id);
    m_runQueueCv.notify_all();
}

//通道接收停泊（Slice 9）：仅状态迁移——等待者集合（含各等待者的结果
//写回指针）在执行器的 ChannelSlot 侧表，唤醒由 send 的直接交付或
//close 的全员唤醒显式触发（WakeForChanRecv）。
void VmScheduler::ParkForChanRecv(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedChanRecv;
    if (t_carrier && t_carrier->currentContextId == id)
        t_carrier->currentContextId = -1;
}

//通道唤醒（Slice 9）：回 Runnable 入队尾。值已由执行器在调用本方法
//之前写入停泊帧的结果槽（停泊的调用不会重执行——pc 推进不变量）。
void VmScheduler::WakeForChanRecv(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Runnable;
    m_runQueue.push_back(id);
    m_runQueueCv.notify_all();
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
