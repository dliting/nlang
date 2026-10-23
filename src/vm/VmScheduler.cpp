/*---
    VmScheduler.cpp — 上下文调度器实现（并发 K1；K2 多载体化 2026-10-20）
    Slice 4 骨架 + Slice 6 join 停泊/唤醒 + Slice 8 互斥停泊/唤醒 + K2：
    条件变量谓词等待、关停通知、死锁检测、每载体状态。定时器关注点
    （sleep 停泊/到期迁移/专用线程）在 VmSchedulerTimer.cpp（K5 task 6
    拆分）。
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
    //K6 D9: a fresh run generation invalidates zombie carriers still
    //parked inside a blocking native from a previous run — they compare
    //after reacquiring the GIL and exit silently instead of truncating
    //the new run's native roots or dereferencing a destroyed context.
    ++m_runGeneration;
    //K6: rendezvous state resets with the run (no in-flight handshake
    //and no waiters exist between runs).
    m_gcStop.Reset();
    //K4：跨 run 残留的调试焦点会饿死新一轮运行的全部其他上下文——
    //每 run 复位；登记表按既定载体数重新置空（定容由
    //ConfigureCarriers 每 run 重申）。
    m_debugFocusContext = -1;
    m_heldByCarrier.assign(m_carrierCount, -1);
    m_gilWaitersPeak.store(0, std::memory_order_relaxed);
    m_gilWaitNanosTotal.store(0, std::memory_order_relaxed);
    m_gilContendedAcquires.store(0, std::memory_order_relaxed);
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
//K4：焦点门出队尝试（须持 GIL）。焦点 Runnable 且在队列中→越队首
//摘出焦点、落账分派并返回其 id；其余一切情形返回 -1（不构成出队）。
int VmScheduler::TryStealFocusedRunnable() {
    if (m_debugFocusContext < 0)
        return -1;
    const ExecutionContext* focus = Find(m_debugFocusContext);
    if (!focus || focus->state != ContextState::Runnable)
        return -1;
    const auto it = std::find(m_runQueue.begin(), m_runQueue.end(),
                              m_debugFocusContext);
    if (it == m_runQueue.end())
        return -1;
    const int id = *it;
    m_runQueue.erase(it);
    DispatchedToCarrier(id);
    return id;
}

//K4：焦点被持时的门等待（须持 GIL，gil 即其锁——wait 期间释放、醒
//来重取）。纯谓词等待门开或关停；返回 false = 关停（调用方必须退出
//出队循环——谓词真后回到焦点臂会空转）。
bool VmScheduler::WaitForHeldFocus(std::unique_lock<std::mutex>& gil) {
    ++m_idleCarrierCount;
    m_runQueueCv.wait(gil, [this] {
        return m_stopRequested || FocusGateOpen();
    });
    --m_idleCarrierCount;
    return !m_stopRequested;
}

//K4：亲和出队（须持 GIL、队列非空）。队首 AFFINITY_SCAN_WINDOW 窗内
//取最早的「lastCarrier==自己 或 255（从未分派，无偏好→匹配任意扫描
//者）」项；未命中回退队首。255 记作全匹配让单载体严格退化为纯 FIFO：
//唯一载体的戳非 0 即 255，一切项都命中，最早命中恒为队首。每次出队
//恰消费一项，位次 ≥ 窗口的项只有在前项被消费后才可及——无饥饿。
int VmScheduler::PopAffinity() {
    const uint8_t self =
        static_cast<uint8_t>(t_carrier->carrierId);
    const size_t window = m_runQueue.size() < AFFINITY_SCAN_WINDOW
        ? m_runQueue.size() : AFFINITY_SCAN_WINDOW;
    auto it = m_runQueue.begin();
    for (size_t i = 0; i < window; ++i, ++it) {
        const ExecutionContext* ctx = Find(*it);
        if (ctx->lastCarrier == 255 || ctx->lastCarrier == self) {
            const int id = *it;
            m_runQueue.erase(it);
            if (ctx->lastCarrier == self)
                ++m_affinityHits;
            else
                ++m_affinityMisses;
            return id;
        }
    }
    ++m_affinityMisses;
    const int id = m_runQueue.front();
    m_runQueue.pop_front();
    return id;
}

int VmScheduler::PopNextRunnable(std::unique_lock<std::mutex>& gil) {
    for (;;) {
        //K4 调试焦点门（D2，见头文件注释）：焦点 Runnable 时它是唯一
        //可分派者。排队中→本次出队取焦点（越过队首非焦点项）；被别的
        //载体持有→无物可分派，等焦点状态迁移（持有者的停泊/终结/重新
        //入队位点都 notify_all）。
        if (m_debugFocusContext >= 0) {
            const ExecutionContext* focus = Find(m_debugFocusContext);
            if (focus && focus->state == ContextState::Runnable) {
                if (const int id = TryStealFocusedRunnable(); id >= 0)
                    return id;
                //焦点被持：不做死锁检测（焦点 Runnable 即存在工作），
                //纯谓词等待门开或关停。关停同空队臂——返回 -1 退出循环
                //（谓词真后 continue 回到焦点臂会空转，必须在此出口）。
                if (!WaitForHeldFocus(gil))
                    return -1;
                continue;
            }
        }
        if (!m_runQueue.empty()) {
            const int id = PopAffinity();
            DispatchedToCarrier(id);
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

//K4：分派落账（须持 GIL）。辅助载体计数沿旧位（出队即记）；唤醒亲
//和的 lastCarrier 戳在此盖（焦点窃取与常规出队共用本位点）。
void VmScheduler::DispatchedToCarrier(int id) {
    t_carrier->currentContextId = id;
    if (ExecutionContext* ctx = FindMutable(id))
        ctx->lastCarrier = static_cast<uint8_t>(t_carrier->carrierId);
    if (t_carrier->carrierId >= 0
        && static_cast<size_t>(t_carrier->carrierId)
               < m_heldByCarrier.size())
        m_heldByCarrier[static_cast<size_t>(t_carrier->carrierId)] = id;
    if (std::this_thread::get_id() != m_primaryThreadId)
        ++m_auxDispatchCount;
}

//K4：窗口结束时清集中登记（须持 GIL）。thread_local 槽先行判断——
//登记表只在持有匹配时才写。
void VmScheduler::ReleaseHeldIfCurrent(int id) {
    if (!t_carrier || t_carrier->currentContextId != id)
        return;
    t_carrier->currentContextId = -1;
    if (t_carrier->carrierId >= 0
        && static_cast<size_t>(t_carrier->carrierId)
               < m_heldByCarrier.size())
        m_heldByCarrier[static_cast<size_t>(t_carrier->carrierId)] = -1;
}

//K4：门放开＝焦点无效/不存在/非 Runnable/已在队列。闭门仅一种形态：
//焦点 Runnable 且被别的载体持有（对持有着本身不构成——它不在出队）。
bool VmScheduler::FocusGateOpen() const {
    if (m_debugFocusContext < 0)
        return true;
    const ExecutionContext* focus = Find(m_debugFocusContext);
    if (!focus || focus->state != ContextState::Runnable)
        return true;
    return std::find(m_runQueue.begin(), m_runQueue.end(),
                     m_debugFocusContext) != m_runQueue.end();
}

void VmScheduler::NotifyGateWaiters() {
    if (m_debugFocusContext >= 0)
        m_runQueueCv.notify_all();
}

void VmScheduler::Requeue(int id) {
    m_runQueue.push_back(id);
    m_runQueueCv.notify_all();
    ReleaseHeldIfCurrent(id);
}

void VmScheduler::MarkFinished(int id, int64_t result) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Finished;
    ctx->resultCell = result;
    ReleaseHeldIfCurrent(id);
    //K4：焦点可能正终结（离开 Runnable）——唤醒门等待者（main 分支的
    //关停通知是重复通知，无害）。
    NotifyGateWaiters();
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
    ReleaseHeldIfCurrent(id);
    //K4：同 MarkFinished——焦点终结唤醒门等待者。
    NotifyGateWaiters();
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
    ReleaseHeldIfCurrent(id);
    //K4：焦点停泊（离开 Runnable）——门等待者须重评（放行）。
    NotifyGateWaiters();
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

//K2 slice 3：析构——置关停、唤醒、join。唯一让定时器线程退出的路径；
//每 run 的关停标志（m_stopRequested）不影响它（常驻执行器，避免每轮
//运行反复创建销毁线程的开销）。定时器关注点的其余实现见
//VmSchedulerTimer.cpp（K5 task 6 拆分）。
VmScheduler::~VmScheduler() {
    {
        std::lock_guard<std::mutex> g(m_gil);
        m_timerShutdown = true;
    }
    m_timerCv.notify_all();
    if (m_timerThread.joinable())
        m_timerThread.join();
}

//互斥停泊（Slice 8）：仅状态迁移——等待者集合在执行器的 MutexSlot 侧
//表（FIFO），唤醒由 unlock 的所有权移交显式触发（WakeForMutex）。
void VmScheduler::ParkForMutex(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedMutex;
    ReleaseHeldIfCurrent(id);
    //K4：焦点停泊——门等待者放行。
    NotifyGateWaiters();
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
    ReleaseHeldIfCurrent(id);
    //K4：焦点停泊——门等待者放行。
    NotifyGateWaiters();
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

//K3 task 4：有界通道发送停泊——仅状态迁移，待发值/属主队列在执行器的
//ChannelSlot 侧表。
void VmScheduler::ParkForChanSend(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedChanSend;
    ReleaseHeldIfCurrent(id);
    //K4：焦点停泊——门等待者放行。
    NotifyGateWaiters();
}

//K3 task 4：发送者唤醒——回 Runnable 入队尾。recv 已把队首待发值挪入
//缓冲（或 close 已预置异常重抛），停泊的调用不会重执行。
void VmScheduler::WakeForChanSend(int id) {
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
//载体求值栈；状态回 Runnable 但不入队（载体持有，同运行中上下文）。
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
