/*---
    VmExecutorOps.cpp — 操作码主分派（ExecuteFunction 骨架：帧守卫/异常收尾 + 纯分派 switch）
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

void VmExecutor::ExecuteFunction(const CompiledFunction& func,
    uint8_t* pResult, uint8_t* locals) {
    if (m_recurseDepth >= RECURSE_LIMIT)
        throw std::runtime_error("NLang VM: recursion limit exceeded");

    //RAII guard: ensures m_recurseDepth is decremented even if an exception
    //is thrown (e.g. division by zero, unknown opcode).
    struct RecurseGuard {
        size_t &depth;
        RecurseGuard(size_t &d) : depth(d) { ++depth; }
        ~RecurseGuard() { --depth; }
    } guard(m_recurseDepth);

    //Push call frame for GC root set. RAII pop on exit.
    m_callStack.push_back({locals, pResult, &func, 0, 0});
    struct FrameGuard {
        std::vector<CallFrame>& stack;
        std::vector<UnwindFrame>& unwind;
        const CompiledFunction* func;
        FrameGuard(std::vector<CallFrame>& s,
                   std::vector<UnwindFrame>& u,
                   const CompiledFunction* f)
            : stack(s), unwind(u), func(f) {}
        ~FrameGuard() {
            //If an exception is propagating, capture this frame for the
            //backtrace before popping. Innermost frame runs first.
            if (std::uncaught_exceptions() > 0 && !stack.empty()) {
                unwind.push_back({func ? func->name : std::string("<unknown>"),
                                  stack.back().currentLine});
            }
            stack.pop_back();
        }
    } frameGuard(m_callStack, m_unwindFrames, &func);

    //Safepoint: function entry is a natural GC point (tempSlot is empty).
    CheckGCSafepoint();

    BytecodeReader reader(func.bytecode.data(), func.bytecode.size());

    //Phase 9d: try/catch dispatch. opPc is declared OUTSIDE the try so the
    //NLangThrow catch block can read the throwing instruction's offset. The
    //for(;;) loop re-enters the try/while after a caught exception seeks the
    //reader to a handler PC.
    uint16_t opPc = 0;
    for(;;) {
    try {
    while (!reader.Eof()) {
        opPc = static_cast<uint16_t>(reader.CurrentOffset());
        OpCode op = reader.ReadOp();

        switch (op) {
        case OpCode::OP_Return:
            return;

        case OpCode::OP_Stop:
            return;

        case OpCode::OP_AssertFail:
            OpAssertFail(reader);

        case OpCode::OP_ConstInt32:
            OpConstInt32(reader, pResult);
            break;

        case OpCode::OP_ConstFloat:
            OpConstFloat(reader, pResult);
            break;

        case OpCode::OP_ConstZero: {
            std::memset(pResult, 0, sizeof(int32_t));
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

        case OpCode::OP_CastIntToFloat:
            OpCastIntToFloat(pResult);
            break;

        case OpCode::OP_CastFloatToInt:
            OpCastFloatToInt(pResult);
            break;

        case OpCode::OP_Int32_to_str:
            OpInt32_to_str(pResult);
            break;
        case OpCode::OP_Float_to_str:
            OpFloat_to_str(pResult);
            break;
        case OpCode::OP_Enum_to_str:
            OpEnum_to_str(reader, pResult);
            break;

        case OpCode::OP_Array_to_str:
            OpArray_to_str(pResult);
            break;

        case OpCode::OP_Add_i32:
            OpAdd_i32(reader, locals);
            break;

        case OpCode::OP_Sub_i32:
            OpSub_i32(reader, locals);
            break;

        case OpCode::OP_Mul_i32:
            OpMul_i32(reader, locals);
            break;

        case OpCode::OP_Div_i32:
            OpDiv_i32(reader, locals);
            break;

        case OpCode::OP_Mod_i32:
            OpMod_i32(reader, locals);
            break;

        case OpCode::OP_Neg_i32:
            OpNeg_i32(reader, locals);
            break;

        case OpCode::OP_Add_f32:
            OpAdd_f32(reader, locals);
            break;

        case OpCode::OP_Sub_f32:
            OpSub_f32(reader, locals);
            break;

        case OpCode::OP_Mul_f32:
            OpMul_f32(reader, locals);
            break;

        case OpCode::OP_Div_f32:
            OpDiv_f32(reader, locals);
            break;

        case OpCode::OP_Neg_f32:
            OpNeg_f32(reader, locals);
            break;

        //Comparison ops: result written to locals[lhs], like arithmetic ops.
        case OpCode::OP_Less_i32:
            OpLess_i32(reader, locals);
            break;

        case OpCode::OP_LessEqual_i32:
            OpLessEqual_i32(reader, locals);
            break;

        case OpCode::OP_Greater_i32:
            OpGreater_i32(reader, locals);
            break;

        case OpCode::OP_GreaterEqual_i32:
            OpGreaterEqual_i32(reader, locals);
            break;

        case OpCode::OP_Equal_i32:
            OpEqual_i32(reader, locals);
            break;

        case OpCode::OP_NotEqual_i32:
            OpNotEqual_i32(reader, locals);
            break;

        case OpCode::OP_Less_f32:
            OpLess_f32(reader, locals);
            break;

        case OpCode::OP_LessEqual_f32:
            OpLessEqual_f32(reader, locals);
            break;

        case OpCode::OP_Greater_f32:
            OpGreater_f32(reader, locals);
            break;

        case OpCode::OP_GreaterEqual_f32:
            OpGreaterEqual_f32(reader, locals);
            break;

        case OpCode::OP_Equal_f32:
            OpEqual_f32(reader, locals);
            break;

        case OpCode::OP_NotEqual_f32:
            OpNotEqual_f32(reader, locals);
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
            OpCallFunc(reader, locals, pResult);
            break;

        // === Phase 13: first-class function values ===
        case OpCode::OP_MakeFunc:
            OpMakeFunc(reader, pResult);
            break;

        case OpCode::OP_CallDelegate:
            OpCallDelegate(reader, locals, pResult);
            break;

        case OpCode::OP_CallDelegateOut:
            OpCallDelegateOut(reader, locals, pResult);
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
        //with plain OP_VarLocal + OP_Assign instructions.
        case OpCode::OP_CallFuncOut:
            OpCallFuncOut(reader, locals, pResult);
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
            OpCallMethodDirect(reader, locals, pResult);
            break;

        //Phase 9e: OP_CallMethodDirect + out writeback (same protocol as
        //OP_CallFuncOut; for methods, slot 0 is `this`, user out params
        //start at slot 1). Out args on virtual dispatch are rejected at
        //compile time, so no OP_CallMethod variant is needed.
        case OpCode::OP_CallMethodDirectOut:
            OpCallMethodDirectOut(reader, locals, pResult);
            break;

        case OpCode::OP_CallMethod:
            OpCallMethod(reader, locals, pResult);
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
    //Fell off the end of bytecode (no explicit OP_Return). Normal exit.
    return;
    }  // end of try
    catch (const NLangThrow& ex) {
        //opPc = offset of the throwing instruction. Scan func.tryBlocks
        //in declaration order; the first match (range + type) handles it.
        bool handled = false;
        for (const auto& tb : func.tryBlocks) {
            if (opPc < tb.startPc || opPc >= tb.endPc)
                continue;
            if (tb.exceptionClassIdx != 0xFFFF &&
                !IsInstanceOrSubclass(ex.heapIdx, tb.exceptionClassIdx))
                continue;
            //Match. Bind catch var, push handler exception (for throw;),
            //clear unwind frames (so a subsequent throw's backtrace starts
            //fresh), and seek the reader to the handler.
            std::memcpy(locals + tb.catchLocalOff, &ex.heapIdx,
                        sizeof(int32_t));
            m_callStack.back().handlerExcStack.push_back(ex.heapIdx);
            m_unwindFrames.clear();
            reader.Seek(tb.handlerPc);
            handled = true;
            break;
        }
        if (!handled)
            throw;  // propagate to outer frame / top-level
        //else: fall through to the for(;;) re-entry, which re-enters try.
    }
    }  // end of for(;;)
}

} // namespace nlang
