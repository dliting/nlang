/*---
    IntrinsicsFuture.cpp — K3 Future<T> 内在函数族（142-143）：
    接收者派发形态（槽 0 = this）。__handle 即上下文 id（与 Thread 同
    布局，无侧表间接）；get 随任务 2 落地。从 IntrinsicsThread.cpp 抽出
    （源文件 500 行预算）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//K3: the typed-spawn face — same layout as Thread (slot 1 = context id,
//no side table); the erased class is one, the type argument lives at the
//call site.
int32_t VmExecutor::MintFutureObject(int ctxId)
{
    if (m_futureClassIdx < 0)
        throw std::runtime_error("NLang VM: Future class not registered");
    int32_t futureObj = AllocClassOnHeap(
        static_cast<uint16_t>(m_futureClassIdx));
    //Write by index — AllocClassOnHeap may have reallocated m_structHeap.
    m_structHeap[static_cast<size_t>(futureObj)][1] = ctxId;
    return futureObj;
}

//K3 shared: resolve a Future receiver (callParamBase slot 0) to its
//context. The handle IS the context id — no side-table indirection; a
//stale or foreign id is a broken instance, not a scheduler state.
const ExecutionContext& VmExecutor::ResolveFutureContext(
    uint16_t callParamBase, uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in Future call");
    const int32_t ctxId =
        m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset];
    const ExecutionContext* ctx = m_spScheduler->Find(ctxId);
    if (!ctx)
        throw std::runtime_error(
            "NLang VM: Future call on an uninitialized instance");
    return *ctx;
}

//future.isDone(): membership probe — Finished/Failed report done (a
//failed future is decided; get() delivers the failure), every other
//state reports pending. Never parks.
bool VmExecutor::IntrinsicFutureIsDone(uint16_t callParamBase,
                                       uint8_t* locals, uint8_t* pResult)
{
    const ExecutionContext& ctx
        = ResolveFutureContext(callParamBase, locals);
    const bool done = ctx.state == ContextState::Finished
        || ctx.state == ContextState::Failed;
    const int32_t r = done ? 1 : 0;
    std::memcpy(pResult, &r, sizeof(r));
    return true;
}

//future.get(): blocking take. Finished copies the target's result cell
//(the raw T bits — OP_Return wrote them; no boxing round-trip), Failed
//rethrows the target's original exception object (join's shape), and a
//still-running target parks the caller on the join mechanism with its
//result destination registered — the finishing context delivers into it
//before the wake (VmScheduler::DeliverJoinResults), so the resumed call
//never re-executes (the pc-advance invariant). A context cannot await
//itself — basic exception, same shape as join's self-join rejection.
bool VmExecutor::IntrinsicFutureGet(uint16_t callParamBase,
                                    uint8_t* locals, uint8_t* pResult)
{
    const ExecutionContext& ctx
        = ResolveFutureContext(callParamBase, locals);
    const int selfId = m_spScheduler->CurrentContextId();
    if (ctx.id == selfId)
        throw std::runtime_error(
            "NLang VM: a future cannot be awaited by its own context");
    if (ctx.state == ContextState::Finished)
    {
        std::memcpy(pResult, &ctx.resultCell, sizeof(ctx.resultCell));
        return true;
    }
    if (ctx.state == ContextState::Failed)
        ThrowJoinTargetFailure(ctx.failExcHeapIdx);
    //Gate BEFORE mutating scheduler state (decision 2 spec (a)): the
    //nested-window rejection must not leave a half-parked context.
    EnsureSuspensionAllowed();
    m_spScheduler->FindMutable(selfId)->joinResultDst = pResult;
    m_spScheduler->ParkForJoin(selfId, ctx.id);
    RequestPark(ParkSignal::Reason::Join);   //[[noreturn]]
}

} // namespace nlang
