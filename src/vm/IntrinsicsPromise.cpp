/*---
    IntrinsicsPromise.cpp — Promise<T> 内在函数族（完成源）：
    ctor 在调度器登记出生停泊的内部上下文（ParkedPromise，无代码帧、
    不入队）并把上下文 id 写入句柄隐藏槽（与 Thread/Future 同布局）；
    future() 铸造指同一上下文的 Future 句柄——全部读侧操作（isDone/
    get）经既有 Future 机制作用于同一决定状态。complete(T)/
    completeExceptionally(Exception) 在调度器锁内完成先到先得的
    决定迁移并唤醒停泊 get 者。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//Promise(): initialize the ALREADY-ALLOCATED receiver (the Semaphore
//ctor protocol — a ctor-call site allocates the instance and passes it
//at slot 0; minting a fresh object here would orphan the receiver and
//leave its __handle zero). Register the born-parked internal context
//and write its id into the receiver's hidden __handle slot — the same
//cell every Thread/Future reader resolves. The context never enters
//the run queue: no root frame, nothing to execute; its state machine
//is driven entirely by the completion intrinsics. Same no-carrier gate
//as spawn: a promise minted in a host call() window could never
//interact with a scheduler (no carrier would ever run the completing
//sides).
bool VmExecutor::IntrinsicPromiseCtor(uint16_t callParamBase,
                                      uint8_t* locals, uint8_t* pResult)
{
    if (m_spScheduler->ActiveCarrierCount() == 0)
        throw std::runtime_error(
            "NLang VM: Promise() requires an active scheduler carrier "
            "(a host call() window cannot register the internal context)");
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in Promise constructor");
    const int ctxId = m_spScheduler->RegisterParkedPromiseContext();
    //Write by index — nothing allocates in between, but the discipline
    //stays uniform with the other handle-writing intrinsics.
    m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset]
        = ctxId;
    //Root the handle on its own context (the Thread/Future shape): the
    //registry arm keeps the object alive for the run's lifetime.
    m_spScheduler->FindMutable(ctxId)->threadObjHeapIdx = thisIdx;
    CheckGCSafepoint();
    return true;
}

//promise.future(): a reader handle to the SAME internal context — every
//isDone/get call site resolves the promise's decision state through the
//ordinary Future mechanism. Repeat calls mint independent handle
//objects over one context (the spec's shared-decision-state contract).
bool VmExecutor::IntrinsicPromiseFuture(uint16_t callParamBase,
                                        uint8_t* locals, uint8_t* pResult)
{
    const ExecutionContext& ctx
        = ResolveFutureContext(callParamBase, locals);
    const int32_t futureObj = MintFutureObject(ctx.id);
    std::memcpy(pResult, &futureObj, sizeof(futureObj));
    CheckGCSafepoint();
    return true;
}

//promise.complete(T value): first-wins transition of the born-parked
//context to Finished. The argument slot is the declared T at the call
//site (an 8-byte frame cell), so the raw T bits land in the result
//cell exactly like OP_Return's write — future.get() copies them back
//with no boxing round-trip. The whole sequence (undecided check,
//finish, deliver, wake) runs under the dispatch arm's single
//scheduler guard, so two racing completes atomically order themselves;
//the loser sees a decided state and returns false. Reentrant windows
//are legal: the transition parks nothing.
bool VmExecutor::IntrinsicPromiseComplete(uint16_t callParamBase,
                                          uint8_t* locals, uint8_t* pResult)
{
    const ExecutionContext& ctx
        = ResolveFutureContext(callParamBase, locals);
    int64_t valueBits;
    std::memcpy(&valueBits, locals + callParamBase + kFrameSlotBytes,
                sizeof(valueBits));
    bool won = false;
    if (ctx.state == ContextState::ParkedPromise)
    {
        //The context's own finishing sequence (VmExecutor.cpp:324-329
        //shape): mark, THEN deliver into parked getters' destinations,
        //then wake — a resumed getter never re-executes (the
        //pc-advance invariant), so the value must be in place first.
        //All under the dispatch arm's guard (first-wins atomicity).
        m_spScheduler->MarkFinished(ctx.id, valueBits);
        m_spScheduler->DeliverJoinResults(ctx.id, valueBits);
        m_spScheduler->WakeJoiners(ctx.id, /*failed=*/false);
        won = true;
    }
    const int32_t r = won ? 1 : 0;
    std::memcpy(pResult, &r, sizeof(r));
    return true;
}

//promise.completeExceptionally(Exception reason): first-wins
//transition to Failed. The exception OBJECT (heap idx) rides the
//argument cell; parked getters rethrow that same object through
//WakeJoiners' failed arm (the join shape — original backtrace
//preserved). An unobserved exceptional completion reaches the run-end
//report (0.10.20): WakeJoiners sets failObserved only when a getter
//was woken.
bool VmExecutor::IntrinsicPromiseCompleteExceptionally(
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    const ExecutionContext& ctx
        = ResolveFutureContext(callParamBase, locals);
    int32_t excIdx;
    std::memcpy(&excIdx, locals + callParamBase + kFrameSlotBytes,
                sizeof(excIdx));
    if (excIdx <= 0
        || static_cast<size_t>(excIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
            "NLang VM: null reference in completeExceptionally");
    bool won = false;
    if (ctx.state == ContextState::ParkedPromise)
    {
        m_spScheduler->MarkFailed(ctx.id, excIdx);
        m_spScheduler->WakeJoiners(ctx.id, /*failed=*/true);
        won = true;
    }
    const int32_t r = won ? 1 : 0;
    std::memcpy(pResult, &r, sizeof(r));
    return true;
}

} // namespace nlang
