#include "VmExecutor.h"
#include <cstring>
#include <cstdio>

namespace nlang {

int VmExecutor::Execute(const CompiledModule& module) {
    int mainIdx = module.FindFunction("main");
    if (mainIdx < 0)
        throw std::runtime_error("NLang VM: no 'main' function found");

    m_currModule = &module;
    m_recurseDepth = 0;

    //Initialize string pool from module's string constants.
    m_stringPool = module.stringConstants;
    //Pool index 0 is reserved for the empty string.
    if (m_stringPool.empty())
        m_stringPool.emplace_back("");

    //Initialize struct heap with sentinel at index 0.
    m_structHeap.clear();
    m_structHeap.emplace_back();  //empty slot at index 0

    //Initialize GC state.
    m_slotKinds.clear();
    m_slotKinds.push_back(0);    //sentinel
    m_slotStructIdx.clear();
    m_slotStructIdx.push_back(0);
    m_freeList.clear();
    m_gcPending = false;
    m_callStack.clear();

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

    //Push call frame for GC root set. RAII pop on exit.
    m_callStack.push_back({locals, pResult, &func});
    struct FrameGuard {
        std::vector<CallFrame>& stack;
        FrameGuard(std::vector<CallFrame>& s) : stack(s) {}
        ~FrameGuard() { stack.pop_back(); }
    } frameGuard(m_callStack);

    //Safepoint: function entry is a natural GC point (tempSlot is empty).
    CheckGCSafepoint();

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

        case OpCode::OP_ConstString: {
            uint16_t poolIdx = reader.ReadUint16();
            int32_t idx = static_cast<int32_t>(poolIdx);
            std::memcpy(pResult, &idx, sizeof(idx));
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
            uint16_t target = reader.ReadUint16();
            //Loop back-edge safepoint: if jumping backward, this is a loop
            //iteration. Check GC here (tempSlot is consumed at this point).
            if (target <= reader.CurrentOffset())
                CheckGCSafepoint();
            reader.Seek(target);
            break;
        }

        case OpCode::OP_JumpIfNot: {
            uint16_t target = reader.ReadUint16();
            uint16_t localOff = reader.ReadUint16();
            int32_t cond;
            std::memcpy(&cond, locals + localOff, sizeof(cond));
            if (!cond)
                reader.Seek(target);
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

        case OpCode::OP_Switch: {
            //Read the switch value local offset — no action needed,
            //the switch value is already in the local slot.
            //Reference: EN's I_Base_Switch.
            reader.ReadUint16();
            break;
        }

        case OpCode::OP_Case: {
            //Read the jump-to-next-handler offset.
            //Reference: EN's I_Base_Case.
            //The jump target is patched by FixChainedJumps at compile time.
            //At runtime, we just read and skip the placeholder — the actual
            //branching is done by OP_JumpIfNot after the condition code.
            reader.ReadUint16();
            break;
        }

        case OpCode::OP_DebugInfo: {
            reader.ReadUint16();
            break;
        }

        case OpCode::OP_Concat_str: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            int32_t idxA, idxB;
            std::memcpy(&idxA, locals + dst, sizeof(idxA));
            std::memcpy(&idxB, locals + src, sizeof(idxB));
            std::string result;
            if (idxA >= 0 && static_cast<size_t>(idxA) < m_stringPool.size())
                result = m_stringPool[static_cast<size_t>(idxA)];
            if (idxB >= 0 && static_cast<size_t>(idxB) < m_stringPool.size())
                result += m_stringPool[static_cast<size_t>(idxB)];
            int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(std::move(result));
            std::memcpy(locals + dst, &newIdx, sizeof(newIdx));
            break;
        }

        case OpCode::OP_Eq_str: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            int32_t idxA, idxB;
            std::memcpy(&idxA, locals + lhs, sizeof(idxA));
            std::memcpy(&idxB, locals + rhs, sizeof(idxB));
            const std::string& a = (idxA >= 0 && static_cast<size_t>(idxA) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(idxA)] : m_stringPool[0];
            const std::string& b = (idxB >= 0 && static_cast<size_t>(idxB) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(idxB)] : m_stringPool[0];
            int32_t r = (a == b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_Ne_str: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            int32_t idxA, idxB;
            std::memcpy(&idxA, locals + lhs, sizeof(idxA));
            std::memcpy(&idxB, locals + rhs, sizeof(idxB));
            const std::string& a = (idxA >= 0 && static_cast<size_t>(idxA) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(idxA)] : m_stringPool[0];
            const std::string& b = (idxB >= 0 && static_cast<size_t>(idxB) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(idxB)] : m_stringPool[0];
            int32_t r = (a != b) ? 1 : 0;
            std::memcpy(locals + lhs, &r, sizeof(r));
            break;
        }

        case OpCode::OP_StrLen: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            int32_t idx;
            std::memcpy(&idx, locals + src, sizeof(idx));
            int32_t len = 0;
            if (idx >= 0 && static_cast<size_t>(idx) < m_stringPool.size())
                //TODO: For UTF-8 support, count code points instead of bytes.
                len = static_cast<int32_t>(m_stringPool[static_cast<size_t>(idx)].size());
            std::memcpy(locals + dst, &len, sizeof(len));
            break;
        }

        case OpCode::OP_AllocStruct: {
            uint16_t dst = reader.ReadUint16();
            uint16_t structIdx = reader.ReadUint16();
            uint16_t fieldCount = reader.ReadUint16();
            int32_t heapIdx = AllocStructOnHeap(structIdx);
            std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
            m_gcPending = true;
            break;
        }

        case OpCode::OP_LoadField: {
            uint16_t dst = reader.ReadUint16();
            uint16_t obj = reader.ReadUint16();
            uint16_t fieldOff = reader.ReadUint16();
            int32_t heapIdx;
            std::memcpy(&heapIdx, locals + obj, sizeof(heapIdx));
            int32_t fieldIdx = static_cast<int32_t>(fieldOff / 4);
            if (heapIdx < 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size()
                || fieldIdx < 0
                || static_cast<size_t>(fieldIdx) >= m_structHeap[static_cast<size_t>(heapIdx)].size())
                throw std::runtime_error("NLang VM: struct field access out of bounds");
            int32_t val = m_structHeap[static_cast<size_t>(heapIdx)][static_cast<size_t>(fieldIdx)];
            std::memcpy(locals + dst, &val, sizeof(val));
            break;
        }

        case OpCode::OP_StoreField: {
            uint16_t obj = reader.ReadUint16();
            uint16_t fieldOff = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            int32_t heapIdx;
            std::memcpy(&heapIdx, locals + obj, sizeof(heapIdx));
            int32_t fieldIdx = static_cast<int32_t>(fieldOff / 4);
            int32_t val;
            std::memcpy(&val, locals + src, sizeof(val));
            if (heapIdx < 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size()
                || fieldIdx < 0
                || static_cast<size_t>(fieldIdx) >= m_structHeap[static_cast<size_t>(heapIdx)].size())
                throw std::runtime_error("NLang VM: struct field store out of bounds");
            m_structHeap[static_cast<size_t>(heapIdx)][static_cast<size_t>(fieldIdx)] = val;
            break;
        }

        case OpCode::OP_New: {
            uint16_t dst = reader.ReadUint16();
            uint16_t classIdx = reader.ReadUint16();
            int32_t heapIdx = AllocClassOnHeap(classIdx);
            std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
            m_gcPending = true;
            break;
        }

        case OpCode::OP_CallMethodDirect: {
            uint16_t funcIndex = reader.ReadUint16();
            uint16_t callParamBase = reader.ReadUint16();
            if (funcIndex >= m_currModule->functions.size())
                throw std::runtime_error("NLang VM: invalid function index in CallMethodDirect");
            const CompiledFunction& callee = m_currModule->functions[funcIndex];
            std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
            uint16_t paramBytes = callee.paramCount * sizeof(int32_t);
            if (paramBytes > 0 && paramBytes <= callee.localsSize)
                std::memcpy(calleeLocals.data(), locals + callParamBase, paramBytes);
            ExecuteFunction(callee, pResult, calleeLocals.data());
            break;
        }

        case OpCode::OP_CallMethod: {
            //Virtual method dispatch — name-based lookup (like EN's
            //I_Base_CallVirtualFunc + FindFunctionChecked).
            uint16_t methodNameIdx = reader.ReadUint16();
            uint16_t callParamBase = reader.ReadUint16();
            if (methodNameIdx >= m_currModule->stringConstants.size())
                throw std::runtime_error("NLang VM: invalid method name string index");
            const std::string& methodName = m_currModule->stringConstants[methodNameIdx];
            //Get this from callParamBase[0].
            int32_t thisHeapIdx;
            std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
            if (thisHeapIdx <= 0 || static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: null reference in CallMethod");
            //Read classIdx from object slot[0].
            int32_t classIdx = m_structHeap[static_cast<size_t>(thisHeapIdx)][0];
            if (classIdx < 0 || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
                throw std::runtime_error("NLang VM: invalid class index in object header");
            //Walk class hierarchy to find the method by name.
            int funcIndex = -1;
            int searchClassIdx = classIdx;
            while (searchClassIdx >= 0 && searchClassIdx < static_cast<int>(m_currModule->classes.size())) {
                const auto& cc = m_currModule->classes[static_cast<size_t>(searchClassIdx)];
                for (uint16_t idx : cc.methodIndices) {
                    if (idx < m_currModule->functions.size()
                        && m_currModule->functions[idx].name == methodName) {
                        funcIndex = idx;
                        break;
                    }
                }
                if (funcIndex >= 0) break;
                searchClassIdx = cc.superClassIdx;
            }
            if (funcIndex < 0)
                throw std::runtime_error("NLang VM: method not found: " + methodName);
            const CompiledFunction& callee = m_currModule->functions[static_cast<size_t>(funcIndex)];
            std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
            uint16_t paramBytes = callee.paramCount * sizeof(int32_t);
            if (paramBytes > 0 && paramBytes <= callee.localsSize)
                std::memcpy(calleeLocals.data(), locals + callParamBase, paramBytes);
            ExecuteFunction(callee, pResult, calleeLocals.data());
            break;
        }

        case OpCode::OP_CallIntrinsic: {
            uint16_t intrinsicId = reader.ReadUint16();
            uint16_t callParamBase = reader.ReadUint16();
            //Phase 3c will implement full intrinsic dispatch.
            //For now, throw since no intrinsics are registered yet.
            (void)intrinsicId;
            (void)callParamBase;
            throw std::runtime_error("NLang VM: intrinsic calls not yet implemented");
        }

        //Hard crash on null — consistent with Java NPE / C# NullReferenceException.
        //EN uses "safe null" (skip + default), but we prefer fail-fast for bug detection.
        case OpCode::OP_NullCheck: {
            uint16_t obj = reader.ReadUint16();
            int32_t heapIdx;
            std::memcpy(&heapIdx, locals + obj, sizeof(heapIdx));
            if (heapIdx <= 0)
                throw std::runtime_error("NLang VM: null reference error");
            break;
        }

        case OpCode::OP_CopyStruct: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            uint16_t structIdx = reader.ReadUint16();
            int32_t srcHeapIdx;
            std::memcpy(&srcHeapIdx, locals + src, sizeof(srcHeapIdx));
            int32_t newHeapIdx = DeepCopyStruct(srcHeapIdx, structIdx);
            std::memcpy(locals + dst, &newHeapIdx, sizeof(newHeapIdx));
            break;
        }

        case OpCode::OP_AllocArray: {
            uint16_t dst = reader.ReadUint16();
            uint16_t arrayTypeIdx = reader.ReadUint16();
            uint16_t sizeSlot = reader.ReadUint16();
            int32_t size;
            std::memcpy(&size, locals + sizeSlot, sizeof(size));
            if (size < 0)
                throw std::runtime_error("NLang VM: negative array size");
            int32_t heapIdx = AllocArrayOnHeap(arrayTypeIdx, size);
            std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
            m_gcPending = true;
            break;
        }

        case OpCode::OP_LoadElement: {
            uint16_t dst = reader.ReadUint16();
            uint16_t arr = reader.ReadUint16();
            uint16_t index = reader.ReadUint16();
            int32_t heapIdx, idx;
            std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
            std::memcpy(&idx, locals + index, sizeof(idx));
            if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: null array access");
            auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
            int32_t length = slot[2];
            if (idx < 0 || idx >= length)
                throw std::runtime_error("NLang VM: array index out of bounds");
            int32_t val = slot[3 + idx];
            std::memcpy(locals + dst, &val, sizeof(val));
            break;
        }

        case OpCode::OP_StoreElement: {
            uint16_t arr = reader.ReadUint16();
            uint16_t index = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            int32_t heapIdx, idx, val;
            std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
            std::memcpy(&idx, locals + index, sizeof(idx));
            std::memcpy(&val, locals + src, sizeof(val));
            if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: null array access");
            auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
            int32_t length = slot[2];
            if (idx < 0 || idx >= length)
                throw std::runtime_error("NLang VM: array index out of bounds");
            slot[3 + idx] = val;
            break;
        }

        case OpCode::OP_ArrayLength: {
            uint16_t dst = reader.ReadUint16();
            uint16_t arr = reader.ReadUint16();
            int32_t heapIdx;
            std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
            if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: null array access");
            int32_t len = m_structHeap[static_cast<size_t>(heapIdx)][2];
            std::memcpy(locals + dst, &len, sizeof(len));
            break;
        }

        default:
            throw std::runtime_error(
                std::string("NLang VM: unknown opcode ") +
                std::to_string(static_cast<int>(op)));
        }
    }
}

int32_t VmExecutor::AllocStructOnHeap(uint16_t structIdx) {
    if (structIdx >= m_currModule->structs.size())
        throw std::runtime_error("NLang VM: invalid struct index in AllocStruct");
    const auto& cs = m_currModule->structs[structIdx];
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(cs.fieldCount, 0);
        m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Struct;
        m_slotStructIdx[static_cast<size_t>(heapIdx)] = structIdx;
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.emplace_back(cs.fieldCount, 0);
        m_slotKinds.push_back(RTK_Struct);
        m_slotStructIdx.push_back(structIdx);
    }
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (cs.fieldTypeKinds[i] == RTK_Struct
            && cs.fieldStructIndices[i] != 0xFFFF) {
            int32_t innerIdx = AllocStructOnHeap(cs.fieldStructIndices[i]);
            m_structHeap[static_cast<size_t>(heapIdx)][i] = innerIdx;
        }
    }
    return heapIdx;
}

int32_t VmExecutor::AllocClassOnHeap(uint16_t classIdx) {
    if (classIdx >= m_currModule->classes.size())
        throw std::runtime_error("NLang VM: invalid class index in AllocClassOnHeap");
    const auto& cc = m_currModule->classes[classIdx];
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(cc.fieldCount + 1, 0);
        m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Class;
        m_slotStructIdx[static_cast<size_t>(heapIdx)] = 0;
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.emplace_back(cc.fieldCount + 1, 0);
        m_slotKinds.push_back(RTK_Class);
        m_slotStructIdx.push_back(0);
    }
    m_structHeap[static_cast<size_t>(heapIdx)][0] = static_cast<int32_t>(classIdx);
    for (uint16_t i = 0; i < cc.fieldCount; ++i) {
        if (cc.fieldTypeKinds[i] == RTK_Struct
            && cc.fieldStructIndices[i] != 0xFFFF) {
            int32_t innerIdx = AllocStructOnHeap(cc.fieldStructIndices[i]);
            m_structHeap[static_cast<size_t>(heapIdx)][i + 1] = innerIdx;
        }
    }
    return heapIdx;
}

int32_t VmExecutor::AllocArrayOnHeap(uint16_t arrayTypeIdx, int32_t size) {
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error("NLang VM: invalid array type index");
    int32_t totalSlots = 3 + size;
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(totalSlots, 0);
        m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Array;
        m_slotStructIdx[static_cast<size_t>(heapIdx)] = arrayTypeIdx;
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.emplace_back(totalSlots, 0);
        m_slotKinds.push_back(RTK_Array);
        m_slotStructIdx.push_back(arrayTypeIdx);
    }
    m_structHeap[static_cast<size_t>(heapIdx)][0] = RTK_Array;
    m_structHeap[static_cast<size_t>(heapIdx)][1] = arrayTypeIdx;
    m_structHeap[static_cast<size_t>(heapIdx)][2] = size;
    return heapIdx;
}

int32_t VmExecutor::DeepCopyStruct(int32_t srcHeapIdx, uint16_t structIdx) {
    if (srcHeapIdx < 0 || static_cast<size_t>(srcHeapIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: invalid struct heap index in CopyStruct");
    if (structIdx >= m_currModule->structs.size())
        throw std::runtime_error("NLang VM: invalid struct index in CopyStruct");
    const auto& cs = m_currModule->structs[structIdx];
    auto srcSlotCopy = m_structHeap[static_cast<size_t>(srcHeapIdx)];
    int32_t newHeapIdx;
    if (!m_freeList.empty()) {
        newHeapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(newHeapIdx)] = srcSlotCopy;
        m_slotKinds[static_cast<size_t>(newHeapIdx)] = RTK_Struct;
        m_slotStructIdx[static_cast<size_t>(newHeapIdx)] = structIdx;
    } else {
        newHeapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.push_back(srcSlotCopy);
        m_slotKinds.push_back(RTK_Struct);
        m_slotStructIdx.push_back(structIdx);
    }
    //Deep-copy struct-typed fields. Class-typed fields are shallow-copied
    //(reference semantics — the index value is copied as-is).
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (cs.fieldTypeKinds[i] == RTK_Struct
            && cs.fieldStructIndices[i] != 0xFFFF) {
            int32_t innerSrcIdx = srcSlotCopy[i];
            int32_t innerNewIdx = DeepCopyStruct(innerSrcIdx,
                cs.fieldStructIndices[i]);
            m_structHeap[static_cast<size_t>(newHeapIdx)][i] = innerNewIdx;
        }
    }
    return newHeapIdx;
}

//GC implementation.

void VmExecutor::CheckGCSafepoint() {
    if (m_gcPending && m_structHeap.size() > m_gcThreshold) {
        m_gcPending = false;
        CollectGarbage();
    }
}

void VmExecutor::CollectGarbage() {
    MarkPhase();
    SweepPhase();
}

void VmExecutor::MarkPhase() {
    m_markBits.assign(m_structHeap.size(), false);
    //Identify root references from all call frames and push to worklist.
    std::vector<int32_t> worklist;
    for (auto& frame : m_callStack) {
        for (auto& ld : frame.func->locals) {
            if (ld.typeKind != RTK_Struct && ld.typeKind != RTK_Class
                && ld.typeKind != RTK_Array)
                continue;
            int32_t val;
            std::memcpy(&val, frame.locals + ld.offset, sizeof(val));
            if (val <= 0 || static_cast<size_t>(val) >= m_slotKinds.size())
                continue;
            if (!m_markBits[val]) {
                m_markBits[val] = true;
                worklist.push_back(val);
            }
        }
        if (frame.pResult) {
            uint8_t retKind = frame.func->returnTypeKind;
            if (retKind == RTK_Class || retKind == RTK_Struct
                || retKind == RTK_Array) {
                int32_t val;
                std::memcpy(&val, frame.pResult, sizeof(val));
                if (val > 0 && static_cast<size_t>(val) < m_slotKinds.size()
                    && !m_markBits[val]) {
                    m_markBits[val] = true;
                    worklist.push_back(val);
                }
            }
        }
    }
    //Iteratively trace references until worklist is empty.
    while (!worklist.empty()) {
        int32_t idx = worklist.back();
        worklist.pop_back();
        if (m_slotKinds[idx] == RTK_Class) {
            int32_t classIdx = m_structHeap[idx][0];
            auto& cc = m_currModule->classes[classIdx];
            for (uint16_t i = 0; i < cc.fieldCount; ++i) {
                int32_t refIdx = m_structHeap[idx][i + 1];
                if (refIdx <= 0 || static_cast<size_t>(refIdx) >= m_slotKinds.size())
                    continue;
                if ((cc.fieldTypeKinds[i] == RTK_Class && m_slotKinds[refIdx] == RTK_Class)
                    || (cc.fieldTypeKinds[i] == RTK_Struct && m_slotKinds[refIdx] == RTK_Struct)) {
                    if (!m_markBits[refIdx]) {
                        m_markBits[refIdx] = true;
                        worklist.push_back(refIdx);
                    }
                }
            }
        } else if (m_slotKinds[idx] == RTK_Struct) {
            uint16_t structIdx = m_slotStructIdx[idx];
            auto& cs = m_currModule->structs[structIdx];
            for (uint16_t i = 0; i < cs.fieldCount; ++i) {
                int32_t refIdx = m_structHeap[idx][i];
                if (refIdx <= 0 || static_cast<size_t>(refIdx) >= m_slotKinds.size())
                    continue;
                if ((cs.fieldTypeKinds[i] == RTK_Class && m_slotKinds[refIdx] == RTK_Class)
                    || (cs.fieldTypeKinds[i] == RTK_Struct && m_slotKinds[refIdx] == RTK_Struct)) {
                    if (!m_markBits[refIdx]) {
                        m_markBits[refIdx] = true;
                        worklist.push_back(refIdx);
                    }
                }
            }
        } else if (m_slotKinds[idx] == RTK_Array) {
            uint16_t arrayTypeIdx = m_slotStructIdx[idx];
            auto& at = m_currModule->arrayTypes[arrayTypeIdx];
            int32_t length = m_structHeap[idx][2];
            for (int32_t i = 0; i < length; ++i) {
                int32_t elemRef = m_structHeap[idx][3 + i];
                if (elemRef <= 0 || static_cast<size_t>(elemRef) >= m_slotKinds.size())
                    continue;
                if (at.elemKind == RTK_Class && m_slotKinds[elemRef] == RTK_Class
                    && !m_markBits[elemRef]) {
                    m_markBits[elemRef] = true;
                    worklist.push_back(elemRef);
                }
                else if (at.elemKind == RTK_Struct && m_slotKinds[elemRef] == RTK_Struct
                         && !m_markBits[elemRef]) {
                    m_markBits[elemRef] = true;
                    worklist.push_back(elemRef);
                }
            }
        }
    }
}

void VmExecutor::SweepPhase() {
    m_freeList.clear();
    for (size_t i = 1; i < m_structHeap.size(); ++i) {
        if (m_slotKinds[i] == 0) continue;
        if (!m_markBits[i]) {
            if (m_slotKinds[i] == RTK_Class)
                FreeOwnedStructs(static_cast<int32_t>(i));
            if (m_slotKinds[i] == RTK_Struct)
                FreeNestedStructs(static_cast<int32_t>(i), m_slotStructIdx[i]);
            if (m_slotKinds[i] == RTK_Array)
                FreeOwnedArrayStructElements(static_cast<int32_t>(i));
            m_structHeap[i].clear();
            m_slotKinds[i] = 0;
            m_freeList.push_back(static_cast<int32_t>(i));
        }
    }
}

void VmExecutor::FreeOwnedStructs(int32_t heapIdx) {
    int32_t classIdx = m_structHeap[heapIdx][0];
    auto& cc = m_currModule->classes[classIdx];
    for (uint16_t i = 0; i < cc.fieldCount; ++i) {
        if (cc.fieldTypeKinds[i] == RTK_Struct
            && cc.fieldStructIndices[i] != 0xFFFF) {
            int32_t structSlotIdx = m_structHeap[heapIdx][i + 1];
            if (structSlotIdx > 0 && static_cast<size_t>(structSlotIdx) < m_slotKinds.size()
                && m_slotKinds[structSlotIdx] == RTK_Struct) {
                FreeNestedStructs(structSlotIdx, cc.fieldStructIndices[i]);
                m_structHeap[structSlotIdx].clear();
                m_slotKinds[structSlotIdx] = 0;
                m_freeList.push_back(structSlotIdx);
            }
        }
    }
}

void VmExecutor::FreeNestedStructs(int32_t heapIdx, uint16_t structIdx) {
    auto& cs = m_currModule->structs[structIdx];
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (cs.fieldTypeKinds[i] == RTK_Struct
            && cs.fieldStructIndices[i] != 0xFFFF) {
            int32_t nestedIdx = m_structHeap[heapIdx][i];
            if (nestedIdx > 0 && static_cast<size_t>(nestedIdx) < m_slotKinds.size()
                && m_slotKinds[nestedIdx] == RTK_Struct) {
                FreeNestedStructs(nestedIdx, cs.fieldStructIndices[i]);
                m_structHeap[nestedIdx].clear();
                m_slotKinds[nestedIdx] = 0;
                m_freeList.push_back(nestedIdx);
            }
        }
    }
}

void VmExecutor::FreeOwnedArrayStructElements(int32_t heapIdx) {
    uint16_t arrayTypeIdx = m_slotStructIdx[heapIdx];
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        return;
    auto& at = m_currModule->arrayTypes[arrayTypeIdx];
    if (at.elemKind != RTK_Struct)
        return;
    int32_t length = m_structHeap[heapIdx][2];
    for (int32_t i = 0; i < length; ++i) {
        int32_t elemRef = m_structHeap[heapIdx][3 + i];
        if (elemRef > 0 && static_cast<size_t>(elemRef) < m_slotKinds.size()
            && m_slotKinds[elemRef] == RTK_Struct) {
            FreeNestedStructs(elemRef, at.elemTypeIdx);
            m_structHeap[elemRef].clear();
            m_slotKinds[elemRef] = 0;
            m_freeList.push_back(elemRef);
        }
    }
}

} // namespace nlang
