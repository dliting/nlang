#include "VmExecutor.h"
#include <cstring>

namespace nlang {

int VmExecutor::Execute(const CompiledModule& module) {
    int mainIdx = module.FindFunction("main");
    if (mainIdx < 0)
        throw std::runtime_error("NLang VM: no 'main' function found");

    m_currModule = &module;
    m_recurseDepth = 0;

    const CompiledFunction& mainFunc = module.functions[mainIdx];
    std::vector<uint8_t> locals(mainFunc.localsSize, 0);
    int32_t result = 0;
    ExecuteFunction(mainFunc, reinterpret_cast<uint8_t*>(&result), locals.data());
    return result;
}

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

    BytecodeReader reader(func.bytecode.data(), func.bytecode.size());

    while (!reader.Eof()) {
        OpCode op = reader.ReadOp();

        switch (op) {
        case OpCode::OP_Return:
            return;

        case OpCode::OP_Stop:
            return;

        case OpCode::OP_ConstInt32: {
            int32_t v = reader.ReadInt32();
            std::memcpy(pResult, &v, sizeof(v));
            break;
        }

        case OpCode::OP_ConstFloat: {
            float v = reader.ReadFloat();
            std::memcpy(pResult, &v, sizeof(v));
            break;
        }

        case OpCode::OP_ConstZero: {
            std::memset(pResult, 0, sizeof(int32_t));
            break;
        }

        case OpCode::OP_VarLocal: {
            uint16_t offset = reader.ReadUint16();
            std::memcpy(pResult, locals + offset, sizeof(int32_t));
            break;
        }

        case OpCode::OP_Assign: {
            uint16_t dstOffset = reader.ReadUint16();
            std::memcpy(locals + dstOffset, pResult, sizeof(int32_t));
            break;
        }

        case OpCode::OP_CastIntToFloat: {
            int32_t iv;
            std::memcpy(&iv, pResult, sizeof(iv));
            float fv = static_cast<float>(iv);
            std::memcpy(pResult, &fv, sizeof(fv));
            break;
        }

        case OpCode::OP_CastFloatToInt: {
            float fv;
            std::memcpy(&fv, pResult, sizeof(fv));
            int32_t iv = static_cast<int32_t>(fv);
            std::memcpy(pResult, &iv, sizeof(iv));
            break;
        }

        case OpCode::OP_Add_i32: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + dst, sizeof(a));
            std::memcpy(&b, locals + src, sizeof(b));
            a += b;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Sub_i32: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + dst, sizeof(a));
            std::memcpy(&b, locals + src, sizeof(b));
            a -= b;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Mul_i32: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + dst, sizeof(a));
            std::memcpy(&b, locals + src, sizeof(b));
            a *= b;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Div_i32: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + dst, sizeof(a));
            std::memcpy(&b, locals + src, sizeof(b));
            if (b == 0)
                throw std::runtime_error("NLang VM: division by zero");
            a /= b;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Mod_i32: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + dst, sizeof(a));
            std::memcpy(&b, locals + src, sizeof(b));
            if (b == 0)
                throw std::runtime_error("NLang VM: modulo by zero");
            a %= b;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Neg_i32: {
            uint16_t dst = reader.ReadUint16();
            int32_t a;
            std::memcpy(&a, locals + dst, sizeof(a));
            a = -a;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Add_f32: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            float a, b;
            std::memcpy(&a, locals + dst, sizeof(a));
            std::memcpy(&b, locals + src, sizeof(b));
            a += b;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Sub_f32: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            float a, b;
            std::memcpy(&a, locals + dst, sizeof(a));
            std::memcpy(&b, locals + src, sizeof(b));
            a -= b;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Mul_f32: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            float a, b;
            std::memcpy(&a, locals + dst, sizeof(a));
            std::memcpy(&b, locals + src, sizeof(b));
            a *= b;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Div_f32: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            float a, b;
            std::memcpy(&a, locals + dst, sizeof(a));
            std::memcpy(&b, locals + src, sizeof(b));
            if (b == 0.0f)
                throw std::runtime_error("NLang VM: division by zero");
            a /= b;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Neg_f32: {
            uint16_t dst = reader.ReadUint16();
            float a;
            std::memcpy(&a, locals + dst, sizeof(a));
            a = -a;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        //Comparison and logical ops: result written to locals[lhs], like arithmetic ops.
        //Reference: EN's IfStmt::Compile pattern.
        case OpCode::OP_Less_i32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a < b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_LessEqual_i32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a <= b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_Greater_i32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a > b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_GreaterEqual_i32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a >= b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_Equal_i32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a == b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_NotEqual_i32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a != b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_Less_f32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            float a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a < b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_LessEqual_f32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            float a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a <= b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_Greater_f32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            float a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a > b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_GreaterEqual_f32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            float a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a >= b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_Equal_f32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            float a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a == b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_NotEqual_f32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            float a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a != b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_LogicalAnd: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a && b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_LogicalOr: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            int32_t a, b;
            std::memcpy(&a, locals + lhs, sizeof(a));
            std::memcpy(&b, locals + rhs, sizeof(b));
            int32_t r = (a || b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_LogicalNot: {
            uint16_t dst = reader.ReadUint16();
            int32_t a;
            std::memcpy(&a, locals + dst, sizeof(a));
            a = !a;
            std::memcpy(locals + dst, &a, sizeof(a));
            break;
        }

        case OpCode::OP_Jump: {
            int16_t target = reader.ReadInt16();
            reader.Seek(static_cast<size_t>(target));
            break;
        }

        case OpCode::OP_JumpIfNot: {
            int16_t target = reader.ReadInt16();
            uint16_t localOff = reader.ReadUint16();
            int32_t cond;
            std::memcpy(&cond, locals + localOff, sizeof(cond));
            if (!cond)
                reader.Seek(static_cast<size_t>(target));
            break;
        }

        case OpCode::OP_CallFunc: {
            uint16_t funcIndex = reader.ReadUint16();
            uint16_t callParamBase = reader.ReadUint16();
            if (funcIndex >= m_currModule->functions.size())
                throw std::runtime_error("NLang VM: invalid function index");
            const CompiledFunction& callee = m_currModule->functions[funcIndex];
            std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
            uint16_t paramBytes = callee.paramCount * sizeof(int32_t);
            if (paramBytes > 0 && paramBytes <= callee.localsSize)
                std::memcpy(calleeLocals.data(), locals + callParamBase, paramBytes);
            ExecuteFunction(callee, pResult, calleeLocals.data());
            break;
        }

        case OpCode::OP_ParaEnd:
            break;

        case OpCode::OP_DebugInfo: {
            reader.ReadUint16();
            break;
        }

        default:
            throw std::runtime_error(
                std::string("NLang VM: unknown opcode ") +
                std::to_string(static_cast<int>(op)));
        }
    }
}

} // namespace nlang
