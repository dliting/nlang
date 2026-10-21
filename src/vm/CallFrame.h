/*---
    CallFrame.h — 帧机调用帧（并发 K1，2026-10-19 从 VmExecutor.h 抽取）
    调度器（VmScheduler.h 的 ExecutionContext）需要按值持有帧栈，独立
    成头避免与 VmExecutor.h 循环包含；内容零变化。
---*/
#pragma once
#include "nlang/vm/CompiledModule.h"
#include <cstdint>
#include <vector>

namespace nlang {

//Call frame stack — the frame machine's substrate and the GC root
//set. One CallFrame per active NLang function invocation.
struct CallFrame {
    //Locals buffer owned by the frame. A per-frame vector: when
    //m_callStack grows, the element move steals the buffer, so every
    //frame's localsBuf.data() stays valid across pushes (raw pointers
    //into a shared buffer would dangle on reallocation).
    std::vector<uint8_t> localsBuf;
    //Result cell threading. externalResult != null → results go to a
    //caller-owned C++ cell (root Execute / toString / host bridge
    //windows). Otherwise resultFrameIdx + resultDstOff name a slot in
    //a frame of this stack — always the cell the CALLER'S frame uses
    //(calls thread the same result cell down), resolved fresh by
    //FrameResultCell.
    uint8_t* externalResult = nullptr;
    int32_t resultFrameIdx = -1;
    uint16_t resultDstOff = 0;
    const CompiledFunction* func = nullptr;
    uint16_t pc = 0;            //resume point: next instruction offset
    uint16_t opPc = 0;          //in-flight instruction offset (tryBlocks range test)
    uint16_t currentLine = 0;   //updated by OP_DebugInfo; 0 = unknown
    uint16_t currentPc = 0;     //ndb: pc of the current statement anchor
    //Out-param writeback armed by the four out call ABIs: bit i marks
    //USER parameter i; on normal return, frame slot (i + outShift)
    //copies back to the caller's callParamBase + i (bound-method
    //delegates shift by one for the receiver). Zero mask = plain call.
    uint32_t outMask = 0;
    uint16_t callParamBase = 0;
    uint8_t outShift = 0;
    //Phase 9d: per-frame stack of currently-caught exception heap idxs.
    //Pushed when a catch handler is entered, popped by OP_PopHandler at
    //catch-block exit. OP_Rethrow reads the top entry to re-raise. Per-
    //frame (not executor-global) so a cross-function call cannot leak a
    //parent's catch context into the callee.
    std::vector<int32_t> handlerExcStack;
};

} // namespace nlang
