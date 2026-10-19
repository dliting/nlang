/*---
    VmScheduler.cpp — 单载体调度器实现（并发 K1 Slice 4 骨架）
    迁移方法一览见 VmScheduler.h；本片无定时器/停泊路径。
---*/
#include "VmScheduler.h"

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

int VmScheduler::PopNextRunnable() {
    if (m_runQueue.empty())
        return -1;
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

const ExecutionContext* VmScheduler::Find(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= m_contexts.size())
        return nullptr;
    return &m_contexts[static_cast<size_t>(id)];
}

ExecutionContext* VmScheduler::FindMutable(int id) {
    return const_cast<ExecutionContext*>(Find(id));
}

} // namespace nlang
