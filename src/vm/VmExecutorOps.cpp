/*---
    VmExecutorOps.cpp — 操作码主分派（帧机：RunFrames 异常收尾 + DispatchFrames 纯分派 switch）
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构）；2026-10-18 并发 K1
    改造为显式帧栈机（零行为变化：NLang 调用不再消耗 C++ 栈，语义/报错/回溯
    逐项保持）。帧构造/弹出/处理器走查在同日抽取至 VmExecutorFrames.cpp。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

//Frame machine core (see VmExecutor.h for the contract group). RunFrames
//owns the exception machinery; DispatchFrames owns the instruction loop.

//K1 Slice 6: a joiner resumed over a FAILED target throws the target's
//original exception object HERE — inside RunFrames' handler-search try,
//so the handler walk covers the just-restored frames with the join call
//site's opPc (exactly as if join had thrown). No-op unless the carrier
//armed the flag (ArmResumeRethrow); consumed at most once per resume.
void VmExecutor::ConsumeResumeThrow() {
    if (ResumeThrowExcIdxRef() == 0)
        return;
    const int32_t excIdx = ResumeThrowExcIdxRef();
    ResumeThrowExcIdxRef() = 0;
    ThrowJoinTargetFailure(excIdx);
}

void VmExecutor::RunFrames(size_t baseDepth)
{
    //K6 D2: this thread executes NLang bytecode for the whole window
    //(carrier window, host CallByName window, nested re-entrant
    //callback window — nesting raises the thread-local depth but
    //counts this thread once). Wrapper form: the 50-line body below
    //has zero headroom under the source-size guard.
    GcWorldStop::ExecWindowGuard evalWindow(m_spScheduler->GcStop());
    RunFramesInner(baseDepth);
}

void VmExecutor::RunFramesInner(size_t baseDepth)
{
    for (;;) {
        std::exception_ptr inflight;
        int32_t inflightExc = 0;
        try {
            ConsumeResumeThrow();
            DispatchFrames(baseDepth);
            return;
        } catch (NLangThrow& ex) {
            //Keep the original exception object alive across the handler
            //search (rethrown by pointer below — no copy; heapIdx is the
            //payload the tryBlocks walk needs frame by frame).
            inflightExc = ex.heapIdx;
            inflight = std::current_exception();
        } catch (ParkSignal&) {
            //Suspension/preemption is not an error (K1 decision 2):
            //frames stay untouched — no unwind records, no handler
            //search — and the carrier loop routes by the signal's
            //reason. Must precede the catch-all below, which would
            //otherwise corrupt UnwindFrames() (the O2 audit's one
            //mandatory ordering).
            throw;
        } catch (ShutdownSignal&) {
            //K2 slice 4: parked frames, not an unwind — pass it on.
            throw;
        } catch (...) {
            //Non-NLang error (unknown opcode, malformed module, ...):
            //record one unwind entry per frame of this window, innermost
            //first, so the uncaught-path backtrace covers these frames
            //too, then propagate unchanged.
            while (EvalStack().size() > baseDepth)
                RecordUnwindAndPopFrame();
            throw;
        }
        //NLang throw in flight: search handlers frame by frame without
        //dispatching instructions. Innermost frame first — the same order
        //the recursive unwind visited frames in.
        while (EvalStack().size() > baseDepth) {
            if (TryEnterCatchHandler(EvalStack().back(), inflightExc)) {
                inflight = nullptr;
                break;
            }
            RecordUnwindAndPopFrame();
        }
        if (inflight)
            std::rethrow_exception(inflight);
        //else: a handler was entered — resume dispatch on the top frame.
    }
}

void VmExecutor::DispatchFrames(size_t baseDepth)
{
    for (;;) {
        //Window drained: every frame above baseDepth has returned.
        if (EvalStack().size() <= baseDepth)
            return;
        //Copy out everything the inner loop reads and let the
        //CallFrame reference die at the end of the block — NO
        //reference may survive across instructions. A false-returning
        //op can still move EvalStack(): a nested evaluation window
        //(user toString via collection formatting, or a native host
        //callback's CallByName) pushes frames, and a vector growth
        //frees the old element array; only the per-frame localsBuf
        //pointers are move-stable. locals/pResult survive by that
        //invariant; a CallFrame& would not (v0.10.0 final review:
        //OP_DebugInfo read opPc through the stale one).
        uint8_t* locals;
        uint8_t* pResult;
        const CompiledFunction* func;
        uint16_t resumePc;
        {
            CallFrame& entryFrame = EvalStack().back();
            locals = entryFrame.localsBuf.data();
            pResult = FrameResultCell(entryFrame);
            func = entryFrame.func;
            resumePc = entryFrame.pc;
        }
        BytecodeReader reader(func->bytecode.data(),
                              func->bytecode.size());
        reader.Seek(resumePc);

        //frameDone = the top frame changed (call pushed / frame returned)
        //and every pointer captured above is suspect — re-fetch on the
        //next outer iteration. The op handlers that can switch frames
        //are exactly the call family (they return true) and the return
        //family below.
        bool frameDone = false;
        while (!reader.Eof() && !frameDone) {
            //Offset of the in-flight instruction: the tryBlocks range
            //test in TryEnterCatchHandler reads it when an op throws,
            //and OP_DebugInfo below consumes it. Stored through a
            //FRESH back() each iteration (the comment above: even
            //false-returning ops may have reallocated) and kept in a
            //local so consumers never touch a stale frame reference.
            const uint16_t opPc =
                static_cast<uint16_t>(reader.CurrentOffset());
            EvalStack().back().opPc = opPc;
            OpCode op = reader.ReadOp();

            switch (op) {
        case OpCode::OP_Return:
        case OpCode::OP_Stop:
            PopFrameWithWriteback();
            frameDone = true;
            break;

        case OpCode::OP_AssertFail:
            OpAssertFail(reader);
            break;

        case OpCode::OP_ConstInt32:
            OpConstInt32(reader, pResult);
            break;

        case OpCode::OP_ConstFloat:
            OpConstFloat(reader, pResult);
            break;

        case OpCode::OP_ConstZero: {
            //Zero the full uniform frame cell so 8-byte kinds read 0.
            std::memset(pResult, 0, kFrameSlotBytes);
            break;
        }

        case OpCode::OP_ConstString:
            OpConstString(reader, pResult);
            break;

        case OpCode::OP_VarLocal:
            OpVarLocal(reader, locals, pResult);
            break;

        case OpCode::OP_Assign:
            OpAssign(reader, locals, pResult);
            break;

        case OpCode::OP_PrimCast:
            OpPrimCast(reader, pResult);
            break;

        case OpCode::OP_Prim_to_str:
            OpPrimToStr(reader, pResult);
            break;
        case OpCode::OP_Enum_to_str:
            OpEnum_to_str(reader, pResult);
            break;

        case OpCode::OP_Array_to_str:
            OpArray_to_str(pResult);
            break;

        //0.7.5 generalized numeric family: kind-immediate ops dispatched
        //through the registry tables (VmExecutorOpsPrim.cpp).
        case OpCode::OP_Add:
            OpAdd(reader, locals);
            break;

        case OpCode::OP_Sub:
            OpSub(reader, locals);
            break;

        case OpCode::OP_Mul:
            OpMul(reader, locals);
            break;

        case OpCode::OP_Div:
            OpDiv(reader, locals);
            break;

        case OpCode::OP_Mod:
            OpMod(reader, locals);
            break;

        case OpCode::OP_Shl:
            OpShl(reader, locals);
            break;

        case OpCode::OP_Shr:
            OpShr(reader, locals);
            break;

        case OpCode::OP_Neg:
            OpNeg(reader, locals);
            break;

        case OpCode::OP_Cmp:
            OpCmp(reader, locals);
            break;

        case OpCode::OP_ConstInt64:
            OpConstInt64(reader, pResult);
            break;

        case OpCode::OP_ConstDouble:
            OpConstDouble(reader, pResult);
            break;

        case OpCode::OP_LogicalNot:
            OpLogicalNot(reader, locals);
            break;

        case OpCode::OP_Jump:
            OpJump(reader);
            break;

        case OpCode::OP_JumpIfNot:
            OpJumpIfNot(reader, locals);
            break;

        case OpCode::OP_CallFunc:
            if (OpCallFunc(reader, locals, pResult))
                frameDone = true;
            break;

        // === Phase 13: first-class function values ===
        case OpCode::OP_MakeFunc:
            OpMakeFunc(reader, pResult);
            break;

        case OpCode::OP_CallDelegate:
            if (OpCallDelegate(reader, locals, pResult))
                frameDone = true;
            break;

        case OpCode::OP_CallDelegateOut:
            if (OpCallDelegateOut(reader, locals, pResult))
                frameDone = true;
            break;

        case OpCode::OP_MakeBoundFunc:
            OpMakeBoundFunc(reader, pResult);
            break;

        case OpCode::OP_MakeVFunc:
            OpMakeVFunc(reader, pResult);
            break;

        case OpCode::OP_Eq_func:
        case OpCode::OP_Ne_func:
            OpFuncEquality(reader, locals, op);
            break;

        case OpCode::OP_Func_to_str:
            OpFunc_to_str(pResult);
            break;

        //Phase 9e: OP_CallFunc + out-parameter writeback. Bit i of
        //outMask marks staging slot i as an out param — after the callee
        //returns, its frame slot i is copied back to the caller's
        //staging area. The backend then spills it into the user variable
        //with plain OP_VarLocal + OP_Assign instructions. The writeback
        //itself now runs in PopFrameWithWriteback (frame machine).
        case OpCode::OP_CallFuncOut:
            if (OpCallFuncOut(reader, locals, pResult))
                frameDone = true;
            break;

        case OpCode::OP_ParaEnd:
            break;

        case OpCode::OP_Switch:
            OpSwitch(reader);
            break;

        case OpCode::OP_Case:
            OpCase(reader);
            break;

        case OpCode::OP_DebugInfo:
            OpDebugInfo(reader, opPc);
            break;

        case OpCode::OP_Concat_str:
            OpConcat_str(reader, locals);
            break;

        case OpCode::OP_Eq_str:
            OpEq_str(reader, locals);
            break;

        case OpCode::OP_Ne_str:
            OpNe_str(reader, locals);
            break;
        //Phase 11 Q4: bytewise relational compare. std::string's
        //operator< is lexicographic on unsigned char values (charTraits
        //compare), so this is memcmp order — and UTF-8 byte order equals
        //code point order, making it correct for multibyte text too.
        //A null operand (handle 0) reads "", same convention as Eq/Ne.
#define STR_REL(OP)                                                    \
        {                                                              \
            uint16_t lhs = reader.ReadUint16();                        \
            uint16_t rhs = reader.ReadUint16();                        \
            int32_t hA, hB;                                            \
            std::memcpy(&hA, locals + lhs, sizeof(hA));                \
            std::memcpy(&hB, locals + rhs, sizeof(hB));                \
            int32_t r = (StrVal(hA) OP StrVal(hB)) ? 1 : 0;            \
            std::memcpy(locals + lhs, &r, sizeof(r));                  \
        }
        case OpCode::OP_Less_str: STR_REL(<); break;
        case OpCode::OP_LessEqual_str: STR_REL(<=); break;
        case OpCode::OP_Greater_str: STR_REL(>); break;
        case OpCode::OP_GreaterEqual_str: STR_REL(>=); break;
#undef STR_REL

        case OpCode::OP_StrLen:
            OpStrLen(reader, locals);
            break;

        case OpCode::OP_StrByteAt:
            OpStrByteAt(reader, locals);
            break;

        case OpCode::OP_StrForeachStep:
            OpStrForeachStep(reader, locals);
            break;

        case OpCode::OP_AllocStruct:
            OpAllocStruct(reader, locals);
            break;

        case OpCode::OP_LoadField:
            OpLoadField(reader, locals);
            break;

        case OpCode::OP_StoreField:
            OpStoreField(reader, locals);
            break;

        case OpCode::OP_New:
            OpNew(reader, locals);
            break;

        case OpCode::OP_CallMethodDirect:
            if (OpCallMethodDirect(reader, locals, pResult))
                frameDone = true;
            break;

        //Phase 9e: OP_CallMethodDirect + out writeback (same protocol as
        //OP_CallFuncOut; for methods, slot 0 is `this`, user out params
        //start at slot 1). Out args on virtual dispatch are rejected at
        //compile time, so no OP_CallMethod variant is needed.
        case OpCode::OP_CallMethodDirectOut:
            if (OpCallMethodDirectOut(reader, locals, pResult))
                frameDone = true;
            break;

        case OpCode::OP_CallMethod:
            if (OpCallMethod(reader, locals, pResult))
                frameDone = true;
            break;

        case OpCode::OP_CallIntrinsic:
            OpCallIntrinsic(reader, locals, pResult);
            break;

        //Hard crash on null — consistent with Java NPE / C# NullReferenceException.
        //Safe-null (skip + default) was considered and rejected:
        //fail-fast surfaces bugs sooner.
        case OpCode::OP_NullCheck:
            OpNullCheck(reader, locals);
            break;

        case OpCode::OP_CopyStruct:
            OpCopyStruct(reader, locals);
            break;

        case OpCode::OP_AllocArray:
            OpAllocArray(reader, locals);
            break;

        case OpCode::OP_LoadElement:
            OpLoadElement(reader, locals);
            break;

        case OpCode::OP_StoreElement:
            OpStoreElement(reader, locals);
            break;

        case OpCode::OP_ArrayLength:
            OpArrayLength(reader, locals);
            break;

        //Phase 8e-1: primitive → Object boxing.
        //Layout: heap slot with m_slotKinds[idx] == RTK_Boxed, slot[0]=typeTag,
        //slot[1]=value bits. Type tag tells OP_Unbox how to unwrap and tells
        //GC not to trace (boxed slots hold no references).
        case OpCode::OP_Box:
            OpBox(reader, pResult);
            break;

        //Phase 8e-1.5: Object → primitive unbox.
        //Reads the boxed-type-tag operand (RTK_Int32/RTK_Float/RTK_String).
        //Heap layout: slot[0] = boxed-type-tag, slot[1] = value bits.
        //Throws if the heap slot isn't boxed or the type tag mismatches.
        case OpCode::OP_Unbox:
            OpUnbox(reader, pResult);
            break;

        //Phase 8e-1.5: class downcast check.
        //Reads the target classIdx operand. Verifies the heap object's
        //runtime class is operand-classIdx or a subclass thereof. Throws
        //on mismatch. Pushes the same heap idx back to pResult on success.
        case OpCode::OP_CheckCast:
            OpCheckCast(reader, pResult);
            break;

        case OpCode::OP_Throw:
            OpThrow(reader, locals);

        case OpCode::OP_Rethrow:
            OpRethrow();

        case OpCode::OP_PopHandler:
            OpPopHandler();
            break;

        default:
            throw std::runtime_error(
                std::string("NLang VM: unknown opcode ") +
                std::to_string(static_cast<int>(op)));
        }
        }
        //Fell off the end of bytecode (no explicit OP_Return). Normal
        //frame exit — same handling as OP_Return.
        if (!frameDone)
            PopFrameWithWriteback();
    }
}

} // namespace nlang
