/*---
    VmBackendEmitExprBinary.cpp — 二元运算表达式发射（含字符串拼接与比较的分派）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>
#include <map>
#include <unordered_set>

namespace nlang {

void VmBackend::Access(SnBinaryExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& bin = static_cast<SnBinaryExpr&>(expr);
        auto op = bin.Op();

        if (op == SnBinaryExpr::OP_Neg || op == SnBinaryExpr::OP_LogicalNot) {
            //Unary: evaluate operand to resultOffset, then negate in-place
            EmitExpression(*bin.Left(), emitter, resultOffset);
            if (op == SnBinaryExpr::OP_Neg) {
                auto* evalType = bin.Left()->EvalDataType();
                if (evalType && evalType->Kind() == NK_Float)
                    emitter.Emit(OpCode::OP_Neg_f32);
                else
                    emitter.Emit(OpCode::OP_Neg_i32);
                emitter.EmitUint16(resultOffset);
            } else {
                emitter.Emit(OpCode::OP_LogicalNot);
                emitter.EmitUint16(resultOffset);
            }
            return;
        }

        //Short-circuit lowering (2026-08-31): && and || must not evaluate
        //the skipped operand. Lowered entirely from existing primitives —
        //OP_JumpIfNot for the branch and double OP_LogicalNot as the raw
        //int32 -> {0,1} normalization. The logical node claims NO
        //eval-area slot: both operands emit into resultOffset
        //sequentially (the right one only on the fall-through path).
        //ExprPeakDepth's BinaryExpr case mirrors this shape — keep them
        //symmetric.
        if (op == SnBinaryExpr::OP_LogicalAnd || op == SnBinaryExpr::OP_LogicalOr)
        {
            auto& logicChildren = bin.Children();
            auto logicIt = logicChildren.begin();
            auto& leftChild = static_cast<SnExpression&>(*logicIt);
            ++logicIt;
            auto& rightChild = static_cast<SnExpression&>(*logicIt);

            if (op == SnBinaryExpr::OP_LogicalAnd) {
                //Invariant: JumpIfNot only jumps when the slot's int32 is
                //exactly 0, so the jump target lands with resultOffset
                //already normalized to 0 — no store needed on the false
                //path, and no trailing Jump (false path and end coincide).
                EmitExpression(leftChild, emitter, resultOffset);
                emitter.Emit(OpCode::OP_JumpIfNot);
                size_t jumpToEndAnd = emitter.CurrentOffset();
                emitter.EmitUint16(0);  //placeholder
                emitter.EmitUint16(resultOffset);
                EmitExpression(rightChild, emitter, resultOffset);
                emitter.Emit(OpCode::OP_LogicalNot);
                emitter.EmitUint16(resultOffset);
                emitter.Emit(OpCode::OP_LogicalNot);
                emitter.EmitUint16(resultOffset);
                emitter.PatchUint16(jumpToEndAnd,
                    static_cast<uint16_t>(emitter.CurrentOffset()));
                return;
            }

            //op == OP_LogicalOr: normalize the LEFT operand first — the
            //true path keeps resultOffset = 1, but a raw truthy left value
            //(e.g. 5) must not leak out as the result.
            EmitExpression(leftChild, emitter, resultOffset);
            emitter.Emit(OpCode::OP_LogicalNot);
            emitter.EmitUint16(resultOffset);
            emitter.Emit(OpCode::OP_LogicalNot);
            emitter.EmitUint16(resultOffset);
            emitter.Emit(OpCode::OP_JumpIfNot);
            size_t jumpToRight = emitter.CurrentOffset();
            emitter.EmitUint16(0);  //placeholder
            emitter.EmitUint16(resultOffset);
            emitter.Emit(OpCode::OP_Jump);
            size_t jumpToEndOr = emitter.CurrentOffset();
            emitter.EmitUint16(0);  //placeholder
            size_t rightStart = emitter.CurrentOffset();
            EmitExpression(rightChild, emitter, resultOffset);
            emitter.Emit(OpCode::OP_LogicalNot);
            emitter.EmitUint16(resultOffset);
            emitter.Emit(OpCode::OP_LogicalNot);
            emitter.EmitUint16(resultOffset);
            emitter.PatchUint16(jumpToRight,
                static_cast<uint16_t>(rightStart));
            emitter.PatchUint16(jumpToEndOr,
                static_cast<uint16_t>(emitter.CurrentOffset()));
            return;
        }

        //Binary: evaluate left to resultOffset, right to a different slot,
        //then apply op. rightSlot must differ from resultOffset to avoid the
        //right operand overwriting the left before the binary op executes.
        //
        //Phase 8e-8: iterate sn.Children() instead of Left()/Right() because
        //resolver may wrap each operand in SnCastExpr for symmetric promotion,
        //after which m_pLeft/m_pRight are stale (still point to the original
        //expression inside the cast). Children()[0]/[1] always reflect the
        //post-wrap tree. Dispatch on Children()[0]'s EvalDataType — for
        //arithmetic with promotion this is the cast target (= T_result); for
        //comparison (no wrap) this is the operand type, which selects the
        //i32/f32/str variant (e.g. OP_Eq_str for string==string even though
        //bin.EvalDataType() is Int32 for all comparisons).
        //Phase 10 audit round-4: park the right operand in a per-level
        //EvalAreaClaim instead of the PickTempSlot chain — the 4-slot chain
        //wraps tempSlot4 → tempSlot at nesting depth 5, silently clobbering
        //the outer parked left operand (`1+(2+(3+(4+(5+6))))` evaluated to
        //25). Claim slots stack per nesting level, so right-nesting depth
        //is unbounded, and claims never collide with resultOffset (claims
        //start at the evalArea cursor; resultOffset always sits below it).
        EvalAreaClaim rightClaim(*this, 1);
        uint16_t rightSlot = rightClaim.base();
        auto& binChildren = bin.Children();
        auto binIt = binChildren.begin();
        auto& leftChild = static_cast<SnExpression&>(*binIt);
        EmitExpression(leftChild, emitter, resultOffset);
        ++binIt;
        EmitExpression(static_cast<SnExpression&>(*binIt), emitter, rightSlot);

        auto* evalType = leftChild.EvalDataType();
        bool isFloat = evalType && evalType->Kind() == NK_Float;
        bool isString = evalType && evalType->Kind() == NK_String;
        //Phase 13: Func operands compare by handle content, not by heap
        //index (no interning) — keyed on the LEFT operand like the other
        //flags; mixed non-null operands are resolver-rejected.
        bool isFunc = evalType && evalType->Kind() == NK_ClassDecl
            && static_cast<SnClassDecl*>(evalType)->IsFuncType();

        switch (op) {
        case SnBinaryExpr::OP_Add:
            if (isString)
                emitter.Emit(OpCode::OP_Concat_str);
            else
                emitter.Emit(isFloat ? OpCode::OP_Add_f32 : OpCode::OP_Add_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Sub:
            emitter.Emit(isFloat ? OpCode::OP_Sub_f32 : OpCode::OP_Sub_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Mul:
            emitter.Emit(isFloat ? OpCode::OP_Mul_f32 : OpCode::OP_Mul_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Div:
            emitter.Emit(isFloat ? OpCode::OP_Div_f32 : OpCode::OP_Div_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Mod:
            emitter.Emit(OpCode::OP_Mod_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Less:
            //Phase 11 Q4: string relational compare is bytewise (UTF-8
            //byte order == code point order). isString keys on the LEFT
            //operand; the resolver guard rejects mixed non-null operands.
            emitter.Emit(isString ? OpCode::OP_Less_str
                : (isFloat ? OpCode::OP_Less_f32 : OpCode::OP_Less_i32));
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_LessEqual:
            emitter.Emit(isString ? OpCode::OP_LessEqual_str
                : (isFloat ? OpCode::OP_LessEqual_f32 : OpCode::OP_LessEqual_i32));
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Greater:
            emitter.Emit(isString ? OpCode::OP_Greater_str
                : (isFloat ? OpCode::OP_Greater_f32 : OpCode::OP_Greater_i32));
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_GreaterEqual:
            emitter.Emit(isString ? OpCode::OP_GreaterEqual_str
                : (isFloat ? OpCode::OP_GreaterEqual_f32 : OpCode::OP_GreaterEqual_i32));
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_Equal:
            if (isFunc)
                emitter.Emit(OpCode::OP_Eq_func);
            else if (isString)
                emitter.Emit(OpCode::OP_Eq_str);
            else
                emitter.Emit(isFloat ? OpCode::OP_Equal_f32 : OpCode::OP_Equal_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        case SnBinaryExpr::OP_NotEqual:
            if (isFunc)
                emitter.Emit(OpCode::OP_Ne_func);
            else if (isString)
                emitter.Emit(OpCode::OP_Ne_str);
            else
                emitter.Emit(isFloat ? OpCode::OP_NotEqual_f32 : OpCode::OP_NotEqual_i32);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(rightSlot);
            break;
        default:
            throw std::runtime_error(
                "NLang backend: unsupported binary operator");
        }
        return;
    }

//Round-12: this used to silently emit ConstZero, converting any
//unhandled expression kind into wrong-but-compiling code. Codegen only
//runs when the front-end saw no errors, so reaching here is an internal
//invariant break — surface it instead of emitting garbage.

} //namespace nlang
