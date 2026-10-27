/*---
    IntrinsicsFuture.cpp — K3 Future<T> 内在函数族（142-143＋静态面 153）：
    接收者派发形态（槽 0 = this）。__handle 即上下文 id（与 Thread 同
    布局，无侧表间接）；get 随任务 2 落地；selectAny（153）为首个静态
    面——无接收者，等待集数组独占槽 0。从 IntrinsicsThread.cpp 抽出
    （源文件 500 行预算）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

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
    if (ctx.state == ContextState::Failed) {
        m_spScheduler->MarkFailureObserved(ctx.id);
        ThrowJoinTargetFailure(ctx.failExcHeapIdx);
    }
    //Gate BEFORE mutating scheduler state (decision 2 spec (a)): the
    //nested-window rejection must not leave a half-parked context.
    EnsureSuspensionAllowed();
    m_spScheduler->FindMutable(selfId)->joinResultDst = pResult;
    m_spScheduler->ParkForJoin(selfId, ctx.id);
    RequestPark(ParkSignal::Reason::Join);   //[[noreturn]]
}

//IntrinsicFutureSelectAny's structural half (split for the 50-line
//budget): read the wait-set array off slot 0, validate the shape, and
//collect the (contextId, index) pairs. Checks in order: non-empty set,
//rank-1 shape, no null elements, no self-await (the get() wording),
//set size within DEFAULT_SELECT_ANY_MAX. Throws on every violation.
//Array access follows the stage-2 unified layout (CompiledModule.h):
//[2]=rank, [3]=elemCount, elements from ArrayHeaderCells(rank) — the
//0.10.x fixed 3-cell header reading broke here in the 0.11 merge.
void VmExecutor::CollectSelectAnyTargets(uint16_t callParamBase,
    uint8_t* locals, int selfId,
    std::vector<std::pair<int, int>>& targets)
{
    int32_t setIdx;
    std::memcpy(&setIdx, locals + callParamBase, sizeof(setIdx));
    if (setIdx <= 0
        || static_cast<size_t>(setIdx) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(setIdx)] != RTK_Array)
        RaiseNlangException(m_nullPtrExcClassIdx,
            "NLang VM: null reference in selectAny");
    auto& setSlot = m_structHeap[static_cast<size_t>(setIdx)];
    const uint8_t rank = static_cast<uint8_t>(setSlot[2]);
    const int32_t count = setSlot[3];
    if (count == 0)
        throw std::runtime_error(
            "NLang VM: selectAny requires at least one future");
    if (count > DEFAULT_SELECT_ANY_MAX)
        throw std::runtime_error("NLang VM: selectAny supports at most "
            + std::to_string(DEFAULT_SELECT_ANY_MAX) + " futures");
    if (rank != 1)
        throw std::runtime_error(
            "NLang VM: selectAny requires a rank-1 future array");
    const uint16_t arrayTypeIdx = static_cast<uint16_t>(setSlot[1]);
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: selectAny on invalid array type");
    const size_t elemBase = ArrayHeaderCells(rank);
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[arrayTypeIdx].elemKind);
    for (int32_t i = 0; i < count; ++i) {
        int32_t futIdx;
        std::memcpy(&futIdx, setSlot.data() + elemBase
            + static_cast<size_t>(i) * cells, sizeof(futIdx));
        if (futIdx <= 0
            || static_cast<size_t>(futIdx) >= m_structHeap.size())
            RaiseNlangException(m_nullPtrExcClassIdx,
                "NLang VM: null future in selectAny");
        auto& future = m_structHeap[static_cast<size_t>(futIdx)];
        const int32_t ctxId = future[kListHandleFieldOffset];
        if (!m_spScheduler->Find(ctxId))
            throw std::runtime_error(
                "NLang VM: Future call on an uninitialized instance");
        if (ctxId == selfId)
            throw std::runtime_error(
                "NLang VM: a future cannot be awaited by its own context");
        targets.emplace_back(ctxId, static_cast<int>(i));
    }
}

//Future.selectAny(Future[] futures) — the STATIC face: no receiver,
//the wait-set array rides slot 0 (the binding row's noReceiver ABI).
//A decided target (Finished or Failed — failure is decided; its
//observation belongs to get()) short-circuits with the minimal such
//index, no suspension needed; an all-pending set parks the caller on
//the join mechanism with joinResultDst as the index destination — the
//wait set registers waiter-side and the winner-side wake (WakeJoiners)
//delivers the index and clears the registration in one lock domain.
bool VmExecutor::IntrinsicFutureSelectAny(uint16_t callParamBase,
                                          uint8_t* locals, uint8_t* pResult)
{
    const int selfId = m_spScheduler->CurrentContextId();
    std::vector<std::pair<int, int>> targets;
    CollectSelectAnyTargets(callParamBase, locals, selfId, targets);
    //Decided-now probe: a read-only take needs no suspension gate
    //(get's fast-path discipline).
    int decided = -1;
    for (const auto& entry : targets) {
        const ExecutionContext* target = m_spScheduler->Find(entry.first);
        if (!target
            || (target->state != ContextState::Finished
                && target->state != ContextState::Failed))
            continue;
        if (decided < 0 || entry.second < decided)
            decided = entry.second;
    }
    if (decided >= 0) {
        std::memcpy(pResult, &decided, sizeof(decided));
        return true;
    }
    //Gate BEFORE mutating scheduler state (get's decision (a)).
    EnsureSuspensionAllowed();
    m_spScheduler->FindMutable(selfId)->joinResultDst = pResult;
    //A target deciding between the probe and the registration
    //short-circuits inside the lock (the destination set above is
    //then unhooked — nothing parked).
    const int won = m_spScheduler->ParkForJoinAny(selfId, targets);
    if (won >= 0) {
        m_spScheduler->FindMutable(selfId)->joinResultDst = nullptr;
        std::memcpy(pResult, &won, sizeof(won));
        return true;
    }
    RequestPark(ParkSignal::Reason::Join);   //[[noreturn]]
}

} // namespace nlang
