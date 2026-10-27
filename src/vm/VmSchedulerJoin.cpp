/*---
    VmSchedulerJoin.cpp — join 域关注点：停泊（单目标/selectAny）、
    失败观测登记、终结唤醒与取值投递。从 VmScheduler.cpp 拆出
    （Future.selectAny 落地后的 500 行预算；全部锁域逻辑原样搬移，
    WakeJoiners 的 selectAny 等待者循环抽出为 WakeSelectAnyWaiters）。
---*/
#include "VmScheduler.h"
#include <cstring>

namespace nlang {

//join 停泊（Slice 6）：状态与目标登记；调用方（载体）随后把帧整栈搬
//入 ctx->frames。不入运行队列——只有目标终结才唤醒。
void VmScheduler::ParkForJoin(int id, int targetId) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedJoin;
    ctx->parkHandle = targetId;
    ReleaseHeldIfCurrent(id);
    //K4：焦点停泊（离开 Runnable）——门等待者须重评（放行）。
    NotifyGateWaiters();
}

//selectAny 停泊（Future.selectAny）：见头文件。锁内复扫覆盖调用方
//探测后的窗口——复扫见决定者即返回下标（调用方可能已过停泊门，无
//害：未停泊、无状态变更）。
int VmScheduler::ParkForJoinAny(int id,
    const std::vector<std::pair<int, int>>& targets) {
    GilEnsureGuard g(*this);
    int won = -1;
    for (const auto& entry : targets) {
        const ExecutionContext* target = Find(entry.first);
        if (!target
            || (target->state != ContextState::Finished
                && target->state != ContextState::Failed))
            continue;
        if (won < 0 || entry.second < won)
            won = entry.second;
    }
    if (won >= 0)
        return won;
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return -1;
    ctx->state = ContextState::ParkedJoin;
    ctx->parkHandle = kSelectAnyNoTarget;
    ctx->selectAnyTargets = targets;
    ReleaseHeldIfCurrent(id);
    NotifyGateWaiters();
    return -1;
}

//0.10.20：join 命中已失败目标的快径重抛前登记观测——目标异常已被
//joiner 接收，收尾报告不得再告警。
void VmScheduler::MarkFailureObserved(int id) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->failObserved = true;
}

//目标终结：全部 ParkedJoin 于该目标的上下文回 Runnable 入队尾；
//failed 目标同时置 rethrowOnResume——joiner 恢复执行前重抛目标的
//原异常对象（保留 raise 时的回溯）。多 joiner 皆唤醒（计划决策 6）。
//failed 且至少唤醒一名 joiner＝失败已被观测（0.10.20 收尾报告据此
//区分「无人 watched」的失败）。selectAny 等待者（parkHandle 不匹配
//任何目标——kSelectAnyNoTarget）由 WakeSelectAnyWaiters 覆盖。
void VmScheduler::WakeJoiners(int targetId, bool failed) {
    GilEnsureGuard g(*this);
    bool anyJoinerWoken = false;
    for (auto& ctx : m_contexts) {
        if (ctx.state != ContextState::ParkedJoin
            || ctx.parkHandle != targetId)
            continue;
        anyJoinerWoken = true;
        ctx.state = ContextState::Runnable;
        //K7 握手：只入队已完成停泊者；停泊展开途中被唤醒者由停泊载体
        //在搬帧临界区内补推（见 ExecutionContext::framesParked）。
        if (ctx.framesParked)
            m_runQueue.push_back(ctx.id);
        if (failed) {
            ctx.rethrowOnResume = true;
            //K3：失败路径不走取值投递，目的地随帧 unwind 作废——
            //立即清空，避免悬垂指针残留注册表。
            ctx.joinResultDst = nullptr;
        }
    }
    WakeSelectAnyWaiters(targetId);
    if (failed && anyJoinerWoken) {
        ExecutionContext* target = FindMutable(targetId);
        if (target)
            target->failObserved = true;
    }
    m_runQueueCv.notify_all();
}

//selectAny 等待者：等待集含本目标即赢家。下标投递、唤醒与登记
//清除同锁完成（登记只驻留等待者一侧，清表即消除其余全部登记——
//Go runtime/select 的等待者自清在全局锁模型下由赢家侧等价承担）。
//失败目标同算决定：赢家收下标、无 rethrow、不置 failObserved
//（值域观察归 get()，未观察的失败仍走收尾报告）。
void VmScheduler::WakeSelectAnyWaiters(int targetId) {
    for (auto& ctx : m_contexts) {
        if (ctx.state != ContextState::ParkedJoin
            || ctx.selectAnyTargets.empty())
            continue;
        int winIdx = -1;
        for (const auto& entry : ctx.selectAnyTargets)
            if (entry.first == targetId) {
                winIdx = entry.second;
                break;
            }
        if (winIdx < 0)
            continue;
        ctx.state = ContextState::Runnable;
        //K7 握手：同上——只入队已完成停泊者。
        if (ctx.framesParked)
            m_runQueue.push_back(ctx.id);
        if (ctx.joinResultDst) {
            std::memcpy(ctx.joinResultDst, &winIdx, sizeof(winIdx));
            ctx.joinResultDst = nullptr;
        }
        ctx.selectAnyTargets.clear();
    }
}

//K3：取值投递（见头文件）。逐停泊者 memcpy kFrameSlotBytes——resultCell
//是 int64_t 统一槽，任何返回种类都按原始 8 字节搬运。join 停泊者
//（joinResultDst 为 null）跳过。
void VmScheduler::DeliverJoinResults(int targetId, int64_t resultCell) {
    GilEnsureGuard g(*this);
    for (auto& ctx : m_contexts) {
        if (ctx.state != ContextState::ParkedJoin
            || ctx.parkHandle != targetId
            || !ctx.joinResultDst)
            continue;
        std::memcpy(ctx.joinResultDst, &resultCell, sizeof(resultCell));
        ctx.joinResultDst = nullptr;
    }
}

} //namespace nlang
