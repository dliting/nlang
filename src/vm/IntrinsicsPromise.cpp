/*---
    IntrinsicsPromise.cpp — Promise<T> 内在函数族（完成源）：
    ctor 在调度器登记出生停泊的内部上下文（ParkedPromise，无代码帧、
    不入队）并把上下文 id 写入句柄隐藏槽（与 Thread/Future 同布局）；
    future() 铸造指同一上下文的 Future 句柄——全部读侧操作（isDone/
    get）经既有 Future 机制作用于同一决定状态。complete()/
    completeExceptionally() 随完成内在函数批落地。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//Promise handle mint: slot 1 = the internal context's id — the identical
//hidden-slot shape MintFutureObject writes, so a promise's future()
//handle and the promise itself resolve to one context.
int32_t VmExecutor::MintPromiseObject(int ctxId)
{
    if (m_promiseClassIdx < 0)
        throw std::runtime_error("NLang VM: Promise class not registered");
    int32_t promiseObj = AllocClassOnHeap(
        static_cast<uint16_t>(m_promiseClassIdx));
    //Write by index — AllocClassOnHeap may have reallocated m_structHeap.
    m_structHeap[static_cast<size_t>(promiseObj)][1] = ctxId;
    return promiseObj;
}

//Promise(): register the born-parked internal context and mint the
//handle over its id. The context never enters the run queue — there is
//no root frame and nothing to execute; its state machine is driven
//entirely by the completion intrinsics. Same no-carrier gate as spawn:
//a promise minted in a host call() window could never interact with a
//scheduler (no carrier would ever run the completing sides).
bool VmExecutor::IntrinsicPromiseCtor(uint16_t callParamBase,
                                      uint8_t* locals, uint8_t* pResult)
{
    if (m_spScheduler->ActiveCarrierCount() == 0)
        throw std::runtime_error(
            "NLang VM: Promise() requires an active scheduler carrier "
            "(a host call() window cannot register the internal context)");
    const int ctxId = m_spScheduler->RegisterParkedPromiseContext();
    ExecutionContext* ctx = m_spScheduler->FindMutable(ctxId);
    //Root the handle on its own context (the Thread/Future shape): join
    //and future() readers resolve through __handle, and the registry arm
    //keeps the object alive for the run's lifetime.
    const int32_t handleObj = MintPromiseObject(ctxId);
    ctx->threadObjHeapIdx = handleObj;
    std::memcpy(pResult, &handleObj, sizeof(handleObj));
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

} // namespace nlang
