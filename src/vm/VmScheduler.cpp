/*---
    VmScheduler.cpp — 上下文调度器实现（并发 K1；K2 多载体化 2026-10-20）
    Slice 4 骨架 + Slice 6 join 停泊/唤醒 + Slice 7 定时器 + Slice 8 互斥
    停泊/唤醒 + K2：条件变量谓词等待、关停通知、死锁检测、每载体状态、
    定时器专用懒线程（K2 slice 3）。
---*/
#include "VmScheduler.h"
#include <algorithm>
#include <cstring>
#include <thread>

namespace nlang {

thread_local VmScheduler::CarrierState VmScheduler::t_selfCarrier;
thread_local VmScheduler::CarrierState* VmScheduler::t_carrier = nullptr;

//GIL 内清空：定时器线程常驻跨 run，其谓词在锁内读 m_timers——无锁的
//clear 会与它数据竞争（K1 单载体时代无此读者）。调用时机在池启动之
//前，锁无竞争。定时器线程本身不重置：谓词见空表即回落静默等待，下
//一轮首个 sleep 登记再唤醒它。
void VmScheduler::Reset() {
    std::lock_guard<std::mutex> g(m_gil);
    m_contexts.clear();
    m_runQueue.clear();
    m_timers.clear();
    m_stopRequested = false;
    m_idleCarrierCount = 0;
    m_blockedNativeCount = 0;
    m_nextId = 0;
    m_auxDispatchCount = 0;
    m_gilWaitersPeak.store(0, std::memory_order_relaxed);
    m_timerCv.notify_all();
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
//取）：全部载体在条件变量上纯谓词等待（无截止时间——定时器到期迁
//移由专用线程负责，其入队经同一条件变量唤醒载体）。死锁检测在进入
//等待前：全部在飞载体都将等待（本载体即将成为最后一个空闲者）且队
//列与定时器表皆空——没有任何唤醒源，直接抛错；main 未完成即无法返
//回，与其静默挂死不如拒绝；定时器表非空则必有未来唤醒，不构成死
//锁。K2 slice 4：BlockedNative 在飞同样是唤醒源（原生可能返回），
//与定时器表同为排除项——原生级死锁（等待永不返回）不在检测范围，
//由原生自身负责。等待中的载体计入 m_idleCarrierCount（锁内维护，
//检测的输入）。
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
        if (m_timers.empty() && m_blockedNativeCount == 0
                && m_idleCarrierCount + 1 >= m_activeCarrierCount.load()) {
            throw std::runtime_error(
                "NLang VM: deadlock — no runnable contexts remain");
        }
        ++m_idleCarrierCount;
        m_runQueueCv.wait(gil, [this] {
            return m_stopRequested || !m_runQueue.empty();
        });
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
        if (failed) {
            ctx.rethrowOnResume = true;
            //K3：失败路径不走取值投递，目的地随帧 unwind 作废——
            //立即清空，避免悬垂指针残留注册表。
            ctx.joinResultDst = nullptr;
        }
        m_runQueue.push_back(ctx.id);
    }
    m_runQueueCv.notify_all();
}

//K3：取值投递（见头文件）。逐停泊者 memcpy kFrameSlotBytes——resultCell
//是 int64_t 统一槽，任何返回种类都按原始 8 字节搬运。join 停泊者
//（joinResultDst 为 null）跳过。
void VmScheduler::DeliverJoinResults(int targetId, int64_t resultCell) {
    for (auto& ctx : m_contexts) {
        if (ctx.state != ContextState::ParkedJoin
            || ctx.parkHandle != targetId
            || !ctx.joinResultDst)
            continue;
        std::memcpy(ctx.joinResultDst, &resultCell, sizeof(resultCell));
        ctx.joinResultDst = nullptr;
    }
}

//sleep 停泊（Slice 7）：状态与唤醒时刻登记、入定时器表；当前上下文
//让出载体。帧的搬运由载体在 ParkSignal 捕获处统一完成。K2 slice 3：
//首个登记懒启动定时器线程（此后常驻执行器），每次登记唤醒它重算最
//近截止。
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
    if (!m_timerThreadStarted) {
        m_timerThreadStarted = true;
        m_timerThread = std::thread([this] { TimerThreadMain(); });
    }
    m_timerCv.notify_all();
}

//到期迁移（见头文件）。线性表规模 = 停泊中的 sleep 上下文数，K1 量级
//下无需堆结构。陈旧条目（状态已非 ParkedTimer）必须丢弃：保留会使
//PopNextRunnable 对一个已过的截止时间无限空转（活锁）。K2 slice 3：
//到期者按到期时刻排序入队——服务方迟到（定时器线程被 GIL 独占饿死、
//或冷启动慢）时多个死线可能同时已过，唤醒顺序须跟随到期时刻而非登记
//序（登记序与睡眠时长序无关）。
void VmScheduler::ExpireDueTimers() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<TimerEntry> remaining;
    std::vector<TimerEntry> due;
    remaining.reserve(m_timers.size());
    for (const TimerEntry& t : m_timers) {
        ExecutionContext* ctx = FindMutable(t.contextId);
        if (!ctx || ctx->state != ContextState::ParkedTimer)
            continue;   //stale — dropped, never re-armed
        if (t.wakeAt <= now)
            due.push_back(t);
        else
            remaining.push_back(t);
    }
    std::stable_sort(due.begin(), due.end(),
        [](const TimerEntry& a, const TimerEntry& b) {
            return a.wakeAt < b.wakeAt;
        });
    for (const TimerEntry& t : due) {
        ExecutionContext* ctx = FindMutable(t.contextId);
        ctx->state = ContextState::Runnable;
        m_runQueue.push_back(ctx->id);
    }
    m_timers = std::move(remaining);
}

//K2 slice 3：析构——置关停、唤醒、join。唯一让定时器线程退出的路径；
//每 run 的关停标志（m_stopRequested）不影响它（常驻执行器，避免每轮
//运行反复创建销毁线程的开销）。
VmScheduler::~VmScheduler() {
    {
        std::lock_guard<std::mutex> g(m_gil);
        m_timerShutdown = true;
    }
    m_timerCv.notify_all();
    if (m_timerThread.joinable())
        m_timerThread.join();
}

std::chrono::steady_clock::time_point VmScheduler::NearestTimerDeadline() const {
    auto nearest = m_timers.front().wakeAt;
    for (const TimerEntry& t : m_timers)
        if (t.wakeAt < nearest)
            nearest = t.wakeAt;
    return nearest;
}

//K2 slice 3：定时器线程体（懒启动，见 ParkForTimer）。外层等待「有定
//时器可管」；内层睡至最近截止。中途醒于三种变化（关停、表被清空、
//更早截止的新登记——以最近截止变化统一识别）则重算继续，否则到期迁
//移并把载体唤醒。陈旧条目若全部被丢弃且无一入队，循环回落外层等待
//（当前语义下停泊中的 sleep 上下文状态不会变，陈旧分支仅为防御）。
void VmScheduler::TimerThreadMain() {
    for (;;) {
        std::unique_lock<std::mutex> gil(m_gil);
        m_timerCv.wait(gil, [this] {
            return m_timerShutdown || !m_timers.empty();
        });
        if (m_timerShutdown)
            return;
        const auto nearest = NearestTimerDeadline();
        const bool deadlineReached = !m_timerCv.wait_until(gil, nearest,
            [this, &nearest] {
                return m_timerShutdown || m_timers.empty()
                    || NearestTimerDeadline() != nearest;
            });
        if (m_timerShutdown)
            return;
        if (!deadlineReached)
            continue;   //表已变：重算最近截止
        ExpireDueTimers();
        gil.unlock();
        m_runQueueCv.notify_all();   //到期上下文已入队：唤醒载体
    }
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

//K2 slice 4（§2.5）：阻塞原生停泊。载体仍持有该上下文（它就是调用
//原生的一方），currentContextId 保持——与 Parked* 家族不同，唤醒不
//经队列事件，而是载体在原生返回后原地续跑。计数供死锁检测排除。
void VmScheduler::MarkBlockedNative(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::BlockedNative;
    ++m_blockedNativeCount;
}

//K2 slice 4（§2.5）：阻塞原生复原——载体已重取 GIL、帧已搬回
//m_callStack；状态回 Runnable 但不入队（载体持有，同运行中上下文）。
void VmScheduler::MarkRunnableHeld(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Runnable;
    --m_blockedNativeCount;
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
