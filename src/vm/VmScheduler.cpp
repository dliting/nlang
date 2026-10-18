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

void VmScheduler::MarkFailed(int id) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::Failed;
    if (m_currentId == id)
        m_currentId = -1;
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
