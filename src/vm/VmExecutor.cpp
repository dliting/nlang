#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

VmExecutor::~VmExecutor() = default;

int VmExecutor::Execute(const CompiledModule& module) {
    //Reset per-run state up front: if the executor is reused (e.g. a
    //future REPL), an early throw below must not expose stale frames
    //or backtrace from a previous Execute() call.
    m_currModule = &module;
    m_recurseDepth = 0;
    m_unwindFrames.clear();
    m_lastBacktrace.clear();
    m_byteStreams.clear();
    m_byteStreamFreeList.clear();
    m_fileStreams.clear();
    m_fileStreamFreeList.clear();
    //Phase 8e-3: reset List<T> side table and cache the class index.
    m_listStore.clear();
    m_listFreeList.clear();
    int listIdx = module.FindClass("List");
    m_listClassIdx = (listIdx >= 0) ? static_cast<int16_t>(listIdx) : -1;

    //Phase 8e-4: reset Dict<K,V> side table and cache the class index.
    m_dictStore.clear();
    m_dictFreeList.clear();
    int dictIdx = module.FindClass("Dict");
    m_dictClassIdx = (dictIdx >= 0) ? static_cast<int16_t>(dictIdx) : -1;

    int mainIdx = module.FindFunction("main");
    if (mainIdx < 0)
        throw std::runtime_error("NLang VM: no 'main' function found");

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
    try {
        ExecuteFunction(mainFunc, reinterpret_cast<uint8_t*>(&result), locals.data());
    } catch (const std::exception&) {
        m_lastBacktrace = FormatBacktrace();
        throw;
    }
    return result;
}

std::string VmExecutor::FormatBacktrace() const {
    std::string out;
    std::string moduleName = m_currModule ? m_currModule->name : "<module>";
    char buf[256];
    //m_unwindFrames is innermost-first (innermost's destructor ran first).
    for (const auto& f : m_unwindFrames) {
        if (f.currentLine != 0) {
            std::snprintf(buf, sizeof(buf), "  at %s (%s.n:%u)\n",
                f.funcName.c_str(), moduleName.c_str(),
                static_cast<unsigned>(f.currentLine));
        } else {
            std::snprintf(buf, sizeof(buf), "  at %s (%s.n:?)\n",
                f.funcName.c_str(), moduleName.c_str());
        }
        out += buf;
    }
    return out;
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
    m_callStack.push_back({locals, pResult, &func, 0});
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

    while (!reader.Eof()) {
        OpCode op = reader.ReadOp();

        switch (op) {
        case OpCode::OP_Return:
            return;

        case OpCode::OP_Stop:
            return;

        case OpCode::OP_AssertFail: {
            uint16_t msgIdx = reader.ReadUint16();
            std::string msg = "assertion failed";
            if (msgIdx < m_stringPool.size() && !m_stringPool[msgIdx].empty())
                msg += ": " + m_stringPool[msgIdx];
            throw std::runtime_error(msg);
        }

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

        case OpCode::OP_Int32_to_str: {
            int32_t iv;
            std::memcpy(&iv, pResult, sizeof(iv));
            std::string s = std::to_string(iv);
            int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(std::move(s));
            std::memcpy(pResult, &newIdx, sizeof(newIdx));
            break;
        }
        case OpCode::OP_Float_to_str: {
            float fv;
            std::memcpy(&fv, pResult, sizeof(fv));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", fv);
            int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(buf);
            std::memcpy(pResult, &newIdx, sizeof(newIdx));
            break;
        }
        case OpCode::OP_Enum_to_str: {
            //uint16 enumDefIdx immediate; read int32 enum value from pResult,
            //lookup m_currModule->enumNames[enumDefIdx][value], push name.
            uint16_t enumDefIdx = reader.ReadUint16();
            int32_t enumValue;
            std::memcpy(&enumValue, pResult, sizeof(enumValue));
            if (enumDefIdx >= m_currModule->enumNames.size())
                throw std::runtime_error(
                    "NLang VM: enum def idx out of range");
            const auto& names = m_currModule->enumNames[enumDefIdx];
            if (enumValue < 0
                || static_cast<size_t>(enumValue) >= names.size())
                throw std::runtime_error(
                    "NLang VM: enum value out of range");
            int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(names[static_cast<size_t>(enumValue)]);
            std::memcpy(pResult, &newIdx, sizeof(newIdx));
            break;
        }

        case OpCode::OP_Array_to_str: {
            //Phase 9b-pre: format array at pResult as "[e1, e2, ...]",
            //push result string to pool, write idx to pResult.
            //Reads heap layout: slot[0]=RTK_Array, slot[1]=elemKind,
            //slot[2]=length, slot[3+i]=elements.
            int32_t heapIdx;
            std::memcpy(&heapIdx, pResult, sizeof(heapIdx));
            std::string s;
            if (heapIdx <= 0) {
                s = "<null>";
            } else if (static_cast<size_t>(heapIdx) >= m_structHeap.size()) {
                throw std::runtime_error(
                    "NLang VM: array_to_str on stale reference");
            } else if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array) {
                throw std::runtime_error(
                    "NLang VM: array_to_str on non-array");
            } else {
                s = FormatArray(heapIdx, 0);
            }
            int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(std::move(s));
            std::memcpy(pResult, &newIdx, sizeof(newIdx));
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
            uint16_t line = reader.ReadUint16();
            if (!m_callStack.empty())
                m_callStack.back().currentLine = line;
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
            if (callee.intrinsicId != INTR_None) {
                ExecuteIntrinsic(callee.intrinsicId, callParamBase, locals, pResult);
                break;
            }
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
            //
            //Note: the null-receiver check below throws *before* the
            //dispatched method's frame is constructed. The backtrace
            //therefore shows the caller (e.g. CallGet) but not the
            //callee (e.g. Get). This is intentional — the callee never
            //ran — and matches how mainstream runtimes report NPEs.
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
            if (callee.intrinsicId != INTR_None) {
                ExecuteIntrinsic(callee.intrinsicId, callParamBase, locals, pResult);
                break;
            }
            std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
            uint16_t paramBytes = callee.paramCount * sizeof(int32_t);
            if (paramBytes > 0 && paramBytes <= callee.localsSize)
                std::memcpy(calleeLocals.data(), locals + callParamBase, paramBytes);
            ExecuteFunction(callee, pResult, calleeLocals.data());
            break;
        }

        case OpCode::OP_CallIntrinsic: {
            //Emitted by VmBackend for string.getHashCode() / string.equals()
            //(strings are primitives without a class, so the call can't go
            //through OP_CallMethod's callee.intrinsicId path). Dispatch
            //reuses the same ExecuteIntrinsic used by the method path.
            uint16_t intrinsicId = reader.ReadUint16();
            uint16_t callParamBase = reader.ReadUint16();
            ExecuteIntrinsic(intrinsicId, callParamBase, locals, pResult);
            break;
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

        //Phase 8e-1: primitive → Object boxing.
        //Layout: heap slot with m_slotKinds[idx] == RTK_Boxed, slot[0]=typeTag,
        //slot[1]=value bits. Type tag tells OP_Unbox how to unwrap and tells
        //GC not to trace (boxed slots hold no references).
        case OpCode::OP_Box: {
            uint8_t typeTag = reader.ReadByte();
            int32_t val;
            std::memcpy(&val, pResult, sizeof(val));
            //Always allocate a heap slot, even for val==0. Null literals never
            //reach OP_Box (they use TCK_Auto, not TCK_Box), so the old
            //null-sentinel optimization (skip allocation for val==0) was
            //incorrectly treating int 0 and float 0.0 as null, breaking
            //List<int>.add(0) and similar paths.
            int32_t heapIdx;
            if (!m_freeList.empty()) {
                heapIdx = m_freeList.back();
                m_freeList.pop_back();
                m_structHeap[static_cast<size_t>(heapIdx)].assign(2, 0);
            } else {
                heapIdx = static_cast<int32_t>(m_structHeap.size());
                m_structHeap.emplace_back(2, 0);
                m_slotKinds.push_back(0);
                m_slotStructIdx.push_back(0);
            }
            m_structHeap[static_cast<size_t>(heapIdx)][0] =
                static_cast<int32_t>(typeTag);
            m_structHeap[static_cast<size_t>(heapIdx)][1] = val;
            m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Boxed;
            m_slotStructIdx[static_cast<size_t>(heapIdx)] = 0;
            std::memcpy(pResult, &heapIdx, sizeof(heapIdx));
            m_gcPending = true;
            break;
        }

        //Phase 8e-1.5: Object → primitive unbox.
        //Reads the boxed-type-tag operand (RTK_Int32/RTK_Float/RTK_String).
        //Heap layout: slot[0] = boxed-type-tag, slot[1] = value bits.
        //Throws if the heap slot isn't boxed or the type tag mismatches.
        case OpCode::OP_Unbox: {
            uint8_t expectedTag = reader.ReadByte();
            int32_t heapIdx;
            std::memcpy(&heapIdx, pResult, sizeof(heapIdx));
            if (heapIdx <= 0
                || static_cast<size_t>(heapIdx) >= m_structHeap.size())
                throw std::runtime_error(
                    "NLang VM: unbox on null/invalid reference");
            if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Boxed)
                throw std::runtime_error(
                    "NLang VM: unbox target is not a boxed primitive");
            int32_t actualTag = m_structHeap[static_cast<size_t>(heapIdx)][0];
            if (static_cast<uint8_t>(actualTag) != expectedTag)
            {
                const char* expName = expectedTag == RTK_Int32  ? "int"  :
                                      expectedTag == RTK_Float  ? "float":
                                      expectedTag == RTK_String ? "string" :
                                      "unknown";
                const char* actName = actualTag == RTK_Int32  ? "int"  :
                                      actualTag == RTK_Float  ? "float":
                                      actualTag == RTK_String ? "string" :
                                      "unknown";
                throw std::runtime_error(std::string(
                    "NLang VM: invalid unbox - expected ") + expName +
                    ", got " + actName);
            }
            int32_t val = m_structHeap[static_cast<size_t>(heapIdx)][1];
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }

        //Phase 8e-1.5: class downcast check.
        //Reads the target classIdx operand. Verifies the heap object's
        //runtime class is operand-classIdx or a subclass thereof. Throws
        //on mismatch. Pushes the same heap idx back to pResult on success.
        case OpCode::OP_CheckCast: {
            uint16_t targetClassIdx = reader.ReadUint16();
            int32_t heapIdx;
            std::memcpy(&heapIdx, pResult, sizeof(heapIdx));
            if (heapIdx <= 0
                || static_cast<size_t>(heapIdx) >= m_structHeap.size())
                throw std::runtime_error(
                    "NLang VM: cast on null/invalid reference");
            if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Class)
                throw std::runtime_error(
                    "NLang VM: CheckCast target is not a class object");
            int32_t actualClassIdx =
                m_structHeap[static_cast<size_t>(heapIdx)][0];
            //Walk the actual class's super chain; accept if target is found.
            bool ok = (actualClassIdx == static_cast<int32_t>(targetClassIdx));
            int32_t cur = actualClassIdx;
            while (!ok && cur > 0)
            {
                const auto& cc = m_currModule->classes[
                    static_cast<size_t>(cur)];
                int16_t sup = cc.superClassIdx;
                if (sup < 0 || static_cast<size_t>(sup) >=
                    m_currModule->classes.size())
                    break;
                if (static_cast<uint16_t>(sup) == targetClassIdx)
                {
                    ok = true;
                    break;
                }
                cur = static_cast<int32_t>(sup);
            }
            if (!ok)
            {
                const auto& tgt = m_currModule->classes[
                    static_cast<size_t>(targetClassIdx)];
                const auto& act = m_currModule->classes[
                    static_cast<size_t>(actualClassIdx)];
                throw std::runtime_error(std::string(
                    "NLang VM: invalid cast - expected `") + tgt.name +
                    "`, got `" + act.name + "`");
            }
            //Result: same heap idx, unchanged.
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

//Phase 8d — polymorphism check for class-typed deserialization.
//Walks the superClassIdx chain from actualIdx upward. Returns true if
//actualIdx is declaredIdx or a subclass thereof. Used by DeserializeClassFields
//and the top-level ReadObject intrinsics to accept stream objects whose
//runtime class is a subclass of the declared (expected) class.
bool VmExecutor::IsSubclassOf(uint16_t actualIdx, uint16_t declaredIdx) {
    int16_t cur = static_cast<int16_t>(actualIdx);
    while (cur >= 0) {
        if (cur == static_cast<int16_t>(declaredIdx)) return true;
        cur = m_currModule->classes[static_cast<size_t>(cur)].superClassIdx;
    }
    return false;
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
            //Phase 8e-3: trace List<T> elements as additional GC roots.
            if (static_cast<int16_t>(classIdx) == m_listClassIdx) {
                int32_t handle = m_structHeap[idx][kListHandleFieldOffset];
                if (handle > 0) {
                    size_t h = static_cast<size_t>(handle - 1);
                    if (h < m_listStore.size()) {
                        for (int32_t elem : m_listStore[h].elements) {
                            if (elem > 0
                                && static_cast<size_t>(elem) < m_slotKinds.size()
                                && !m_markBits[elem]) {
                                auto k = m_slotKinds[elem];
                                if (k == RTK_Class || k == RTK_Struct
                                    || k == RTK_Boxed) {
                                    m_markBits[elem] = true;
                                    if (k == RTK_Class || k == RTK_Struct)
                                        worklist.push_back(elem);
                                }
                            }
                        }
                    }
                }
            }
            //Phase 8e-4: trace Dict<K,V> entries (both K and V are heap idxs).
            if (static_cast<int16_t>(classIdx) == m_dictClassIdx) {
                int32_t handle = m_structHeap[idx][kListHandleFieldOffset];
                if (handle > 0) {
                    size_t h = static_cast<size_t>(handle - 1);
                    if (h < m_dictStore.size()) {
                        for (auto& kv : m_dictStore[h].entries) {
                            for (int32_t elem : {kv.first, kv.second}) {
                                if (elem > 0
                                    && static_cast<size_t>(elem) < m_slotKinds.size()
                                    && !m_markBits[elem]) {
                                    auto k = m_slotKinds[elem];
                                    if (k == RTK_Class || k == RTK_Struct
                                        || k == RTK_Boxed) {
                                        m_markBits[elem] = true;
                                        if (k == RTK_Class || k == RTK_Struct)
                                            worklist.push_back(elem);
                                    }
                                }
                            }
                        }
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
            if (m_slotKinds[i] == RTK_Class) {
                //Phase 8e-3: recycle List<T> handle when a List instance is freed.
                int32_t classIdx = m_structHeap[i][0];
                if (static_cast<int16_t>(classIdx) == m_listClassIdx) {
                    int32_t handle = m_structHeap[i][kListHandleFieldOffset];
                    if (handle > 0)
                        m_listFreeList.push_back(handle);
                }
                //Phase 8e-4: recycle Dict<K,V> handle when a Dict instance is freed.
                if (static_cast<int16_t>(classIdx) == m_dictClassIdx) {
                    int32_t handle = m_structHeap[i][kListHandleFieldOffset];
                    if (handle > 0)
                        m_dictFreeList.push_back(handle);
                }
                FreeOwnedStructs(static_cast<int32_t>(i));
            }
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

//Phase 8b struct serialization helpers.
//SerializeStructFields: walks a struct's fields and emits bytes via the Writer.
//Class fields recurse via SerializeClassFields (Phase 8c); array fields throw
//(Phase 8e). Depth limit prevents pathological cycles
//(shouldn't happen since structs are value types with no cycles, but defends
//against bugs and future reference-field features).
template<typename Writer, typename StreamState>
void VmExecutor::SerializeStructFields(int32_t heapIdx, uint16_t structIdx,
    Writer&& write, StreamState& st, int depth)
{
    if (depth >= static_cast<int>(STRUCT_SERIALIZE_DEPTH_LIMIT))
        throw std::runtime_error("NLang VM: struct serialize depth limit exceeded");
    if (structIdx >= m_currModule->structs.size())
        throw std::runtime_error("NLang VM: invalid struct index in SerializeStructFields");
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: invalid struct heap index in SerializeStructFields");
    const auto& cs = m_currModule->structs[structIdx];
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    for (uint16_t i = 0; i < cs.fieldCount; ++i)
    {
        uint16_t ftk = cs.fieldTypeKinds[i];
        if (ftk == RTK_Int32 || ftk == RTK_Float)
        {
            int32_t val = slot[i];
            uint8_t bytes[4];
            std::memcpy(bytes, &val, 4);
            write(bytes, 4);
        }
        else if (ftk == RTK_String)
        {
            int32_t strIdx = slot[i];
            const std::string& s = (strIdx >= 0
                && static_cast<size_t>(strIdx) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(strIdx)] : "";
            int32_t len = static_cast<int32_t>(s.size());
            uint8_t lenBytes[4];
            std::memcpy(lenBytes, &len, 4);
            write(lenBytes, 4);
            write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        }
        else if (ftk == RTK_Struct)
        {
            if (cs.fieldStructIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested struct type");
            int32_t innerHeapIdx = slot[i];
            SerializeStructFields(innerHeapIdx, cs.fieldStructIndices[i],
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_Class)
        {
            if (cs.fieldClassIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested class type");
            int32_t childHeapIdx = slot[i];
            SerializeClassFields(childHeapIdx,
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_Array)
        {
            throw std::runtime_error(
                "NLang VM: WriteStruct does not support array fields (Phase 8e)");
        }
    }
}

template<typename Reader, typename StreamState>
void VmExecutor::DeserializeStructFields(int32_t heapIdx, uint16_t structIdx,
    Reader&& read, StreamState& st, int depth)
{
    if (depth >= static_cast<int>(STRUCT_SERIALIZE_DEPTH_LIMIT))
        throw std::runtime_error("NLang VM: struct serialize depth limit exceeded");
    if (structIdx >= m_currModule->structs.size())
        throw std::runtime_error("NLang VM: invalid struct index in DeserializeStructFields");
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: invalid struct heap index in DeserializeStructFields");
    const auto& cs = m_currModule->structs[structIdx];
    size_t heapIdxSz = static_cast<size_t>(heapIdx);
    for (uint16_t i = 0; i < cs.fieldCount; ++i)
    {
        uint16_t ftk = cs.fieldTypeKinds[i];
        if (ftk == RTK_Int32 || ftk == RTK_Float)
        {
            uint8_t bytes[4];
            read(bytes, 4);
            int32_t val;
            std::memcpy(&val, bytes, 4);
            m_structHeap[heapIdxSz][i] = val;
        }
        else if (ftk == RTK_String)
        {
            uint8_t lenBytes[4];
            read(lenBytes, 4);
            int32_t len;
            std::memcpy(&len, lenBytes, 4);
            if (len < 0)
                throw std::runtime_error(
                    "NLang VM: ReadStruct string length negative ("
                    + std::to_string(len) + ")");
            if (static_cast<size_t>(len) > MAX_STRING_LENGTH)
                throw std::runtime_error(
                    "NLang VM: ReadStruct string length exceeds cap ("
                    + std::to_string(len) + " > "
                    + std::to_string(MAX_STRING_LENGTH) + ")");
            std::string s(static_cast<size_t>(len), '\0');
            if (len > 0)
                read(reinterpret_cast<uint8_t*>(&s[0]),
                    static_cast<size_t>(len));
            int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(std::move(s));
            m_structHeap[heapIdxSz][i] = newIdx;
        }
        else if (ftk == RTK_Struct)
        {
            if (cs.fieldStructIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested struct type");
            int32_t innerHeapIdx = AllocStructOnHeap(cs.fieldStructIndices[i]);
            m_structHeap[heapIdxSz][i] = innerHeapIdx;
            DeserializeStructFields(innerHeapIdx, cs.fieldStructIndices[i],
                std::forward<Reader>(read), st, depth + 1);
        }
        else if (ftk == RTK_Class)
        {
            if (cs.fieldClassIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested class type");
            int32_t childHeapIdx = 0;
            DeserializeClassFields(cs.fieldClassIndices[i], childHeapIdx,
                std::forward<Reader>(read), st, depth + 1);
            m_structHeap[heapIdxSz][i] = childHeapIdx;
        }
        else if (ftk == RTK_Array)
        {
            throw std::runtime_error(
                "NLang VM: ReadStruct does not support array fields (Phase 8e)");
        }
    }
}

//Phase 8c class serialization helpers.
//SerializeClassFields: emits a tagged record for a class-typed reference.
//tag=0: null (heapIdx <= 0). tag=1: new object (class name + field payload),
//recorded in st.serializeObjIds for later back-references. tag=2: back-ref
//to a previously-emitted object id. Per-kind field switch mirrors
//SerializeStructFields but reads from cc.fieldTypeKinds[i] / slot[i+1]
//(class header occupies slot[0]).
template<typename Writer, typename StreamState>
void VmExecutor::SerializeClassFields(int32_t heapIdx,
    Writer&& write, StreamState& st, int depth)
{
    if (depth >= static_cast<int>(STRUCT_SERIALIZE_DEPTH_LIMIT))
        throw std::runtime_error("NLang VM: struct serialize depth limit exceeded");

    if (heapIdx <= 0) {
        uint8_t tag = 0;
        write(&tag, 1);
        return;
    }
    if (static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: invalid class heap index in SerializeClassFields");

    auto it = st.serializeObjIds.find(heapIdx);
    if (it != st.serializeObjIds.end()) {
        uint8_t tag = 2;
        write(&tag, 1);
        uint32_t id = it->second;
        write(reinterpret_cast<const uint8_t*>(&id), 4);
        return;
    }

    uint32_t id = st.nextObjId++;
    st.serializeObjIds[heapIdx] = id;

    uint8_t tag = 1;
    write(&tag, 1);

    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    int32_t classIdx = slot[0];
    if (classIdx < 0 || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
        throw std::runtime_error("NLang VM: invalid class index in class heap slot");
    const auto& cc = m_currModule->classes[static_cast<size_t>(classIdx)];

    uint32_t nameLen = static_cast<uint32_t>(cc.name.size());
    write(reinterpret_cast<const uint8_t*>(&nameLen), 4);
    write(reinterpret_cast<const uint8_t*>(cc.name.data()), nameLen);

    for (uint16_t i = 0; i < cc.fieldCount; ++i)
    {
        uint16_t ftk = cc.fieldTypeKinds[i];
        if (ftk == RTK_Int32 || ftk == RTK_Float)
        {
            int32_t val = slot[i + 1];
            uint8_t bytes[4];
            std::memcpy(bytes, &val, 4);
            write(bytes, 4);
        }
        else if (ftk == RTK_String)
        {
            int32_t strIdx = slot[i + 1];
            const std::string& s = (strIdx >= 0
                && static_cast<size_t>(strIdx) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(strIdx)] : "";
            int32_t len = static_cast<int32_t>(s.size());
            uint8_t lenBytes[4];
            std::memcpy(lenBytes, &len, 4);
            write(lenBytes, 4);
            write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        }
        else if (ftk == RTK_Struct)
        {
            if (cc.fieldStructIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested struct type");
            int32_t innerHeapIdx = slot[i + 1];
            SerializeStructFields(innerHeapIdx, cc.fieldStructIndices[i],
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_Class)
        {
            if (cc.fieldClassIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested class type");
            int32_t childHeapIdx = slot[i + 1];
            SerializeClassFields(childHeapIdx,
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_Array)
        {
            throw std::runtime_error(
                "NLang VM: WriteStruct does not support array fields (Phase 8e)");
        }
    }
}

//DeserializeClassFields: reads a tagged record written by SerializeClassFields
//and produces a freshly-allocated class object (or back-reference). On tag=1,
//the object is registered in st.deserializeObjIds before fields are decoded
//so cycles back to this object resolve correctly. Per-kind field switch
//mirrors DeserializeStructFields but writes to slot[i+1].
template<typename Reader, typename StreamState>
void VmExecutor::DeserializeClassFields(uint16_t declaredClassIdx,
    int32_t& outHeapIdx, Reader&& read, StreamState& st, int depth)
{
    if (depth >= static_cast<int>(STRUCT_SERIALIZE_DEPTH_LIMIT))
        throw std::runtime_error("NLang VM: struct serialize depth limit exceeded");

    uint8_t tag;
    read(&tag, 1);

    if (tag == 0) {
        outHeapIdx = 0;
        return;
    }
    if (tag == 2) {
        uint32_t id;
        read(reinterpret_cast<uint8_t*>(&id), 4);
        auto it = st.deserializeObjIds.find(id);
        if (it == st.deserializeObjIds.end())
            throw std::runtime_error(
                "NLang VM: ReadStruct back-reference to unknown object id");
        outHeapIdx = it->second;
        return;
    }
    if (tag != 1)
        throw std::runtime_error("NLang VM: ReadStruct unknown class ref tag");

    uint32_t nameLen;
    read(reinterpret_cast<uint8_t*>(&nameLen), 4);
    if (static_cast<size_t>(nameLen) > MAX_STRING_LENGTH)
        throw std::runtime_error(
            "NLang VM: ReadStruct class name length exceeds cap ("
            + std::to_string(nameLen) + " > "
            + std::to_string(MAX_STRING_LENGTH) + ")");
    std::string className(static_cast<size_t>(nameLen), '\0');
    if (nameLen > 0)
        read(reinterpret_cast<uint8_t*>(&className[0]), nameLen);

    int classIdx = m_currModule->FindClass(className);
    if (classIdx < 0)
        throw std::runtime_error(
            "NLang VM: class not found in stream: " + className);
    if (!IsSubclassOf(static_cast<uint16_t>(classIdx), declaredClassIdx)) {
        const std::string& declaredName =
            m_currModule->classes[declaredClassIdx].name;
        throw std::runtime_error(
            "NLang VM: class type mismatch: expected "
            + declaredName + ", got " + className);
    }

    int32_t heapIdx = AllocClassOnHeap(static_cast<uint16_t>(classIdx));
    st.deserializeObjIds[st.nextObjId++] = heapIdx;

    const auto& cc = m_currModule->classes[static_cast<size_t>(classIdx)];
    size_t heapIdxSz = static_cast<size_t>(heapIdx);
    for (uint16_t i = 0; i < cc.fieldCount; ++i)
    {
        uint16_t ftk = cc.fieldTypeKinds[i];
        if (ftk == RTK_Int32 || ftk == RTK_Float)
        {
            uint8_t bytes[4];
            read(bytes, 4);
            int32_t val;
            std::memcpy(&val, bytes, 4);
            m_structHeap[heapIdxSz][i + 1] = val;
        }
        else if (ftk == RTK_String)
        {
            uint8_t lenBytes[4];
            read(lenBytes, 4);
            int32_t len;
            std::memcpy(&len, lenBytes, 4);
            if (len < 0)
                throw std::runtime_error(
                    "NLang VM: ReadStruct string length negative ("
                    + std::to_string(len) + ")");
            if (static_cast<size_t>(len) > MAX_STRING_LENGTH)
                throw std::runtime_error(
                    "NLang VM: ReadStruct string length exceeds cap ("
                    + std::to_string(len) + " > "
                    + std::to_string(MAX_STRING_LENGTH) + ")");
            std::string s(static_cast<size_t>(len), '\0');
            if (len > 0)
                read(reinterpret_cast<uint8_t*>(&s[0]),
                    static_cast<size_t>(len));
            int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(std::move(s));
            m_structHeap[heapIdxSz][i + 1] = newIdx;
        }
        else if (ftk == RTK_Struct)
        {
            if (cc.fieldStructIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested struct type");
            int32_t innerHeapIdx = AllocStructOnHeap(cc.fieldStructIndices[i]);
            m_structHeap[heapIdxSz][i + 1] = innerHeapIdx;
            DeserializeStructFields(innerHeapIdx, cc.fieldStructIndices[i],
                std::forward<Reader>(read), st, depth + 1);
        }
        else if (ftk == RTK_Class)
        {
            if (cc.fieldClassIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested class type");
            int32_t childHeapIdx = 0;
            DeserializeClassFields(cc.fieldClassIndices[i], childHeapIdx,
                std::forward<Reader>(read), st, depth + 1);
            m_structHeap[heapIdxSz][i + 1] = childHeapIdx;
        }
        else if (ftk == RTK_Array)
        {
            throw std::runtime_error(
                "NLang VM: ReadStruct does not support array fields (Phase 8e)");
        }
    }

    outHeapIdx = heapIdx;
}

} // namespace nlang

//Stub implementations — will be filled in Step 7.
namespace nlang {

int32_t VmExecutor::AllocByteStreamHandle() {
    if (!m_byteStreamFreeList.empty()) {
        int32_t h = m_byteStreamFreeList.back();
        m_byteStreamFreeList.pop_back();
        m_byteStreams[static_cast<size_t>(h) - 1] = std::make_unique<ByteStreamState>();
        return h;
    }
    int32_t h = static_cast<int32_t>(m_byteStreams.size()) + 1;
    m_byteStreams.push_back(std::make_unique<ByteStreamState>());
    return h;
}

int32_t VmExecutor::AllocFileStreamHandle() {
    if (!m_fileStreamFreeList.empty()) {
        int32_t h = m_fileStreamFreeList.back();
        m_fileStreamFreeList.pop_back();
        m_fileStreams[static_cast<size_t>(h) - 1] = std::make_unique<FileStreamState>();
        return h;
    }
    int32_t h = static_cast<int32_t>(m_fileStreams.size()) + 1;
    m_fileStreams.push_back(std::make_unique<FileStreamState>());
    return h;
}

//Phase 8e-3: allocate a handle from the List<T> side table.
//Handles are 1-based; __handle==0 means null (uninitialized).
int32_t VmExecutor::AllocListHandle() {
    if (!m_listFreeList.empty()) {
        int32_t h = m_listFreeList.back();
        m_listFreeList.pop_back();
        m_listStore[h - 1] = ListSlot{};
        return h;
    }
    int32_t h = static_cast<int32_t>(m_listStore.size()) + 1;
    m_listStore.emplace_back();
    return h;
}

//Phase 8e-4: Dict<K,V> helpers — mirror List's shape.
int32_t VmExecutor::AllocDictHandle() {
    if (!m_dictFreeList.empty()) {
        int32_t h = m_dictFreeList.back();
        m_dictFreeList.pop_back();
        m_dictStore[h - 1] = DictSlot{};
        return h;
    }
    int32_t h = static_cast<int32_t>(m_dictStore.size()) + 1;
    m_dictStore.emplace_back();
    return h;
}

int32_t VmExecutor::ReadDictHandle(uint16_t callParamBase, uint8_t* locals,
    const char* methodName)
{
    int32_t thisHeapIdx;
    std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
    if (thisHeapIdx <= 0)
        throw std::runtime_error(
            std::string("NLang VM: Dict ") + methodName + " on null instance");
    if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            std::string("NLang VM: Dict ") + methodName + " on stale reference");
    int32_t handle = m_structHeap[static_cast<size_t>(thisHeapIdx)]
        [kListHandleFieldOffset];
    if (handle <= 0)
        throw std::runtime_error(
            std::string("NLang VM: Dict ") + methodName +
            " on uninitialized instance");
    return handle;
}

bool VmExecutor::DictKeysEqual(int32_t k1, int32_t k2) const {
    //Identity fast path (also handles k1==k2==0/null).
    if (k1 == k2) return true;
    if (k1 <= 0 || k2 <= 0) return false;
    if (static_cast<size_t>(k1) >= m_slotKinds.size()
        || static_cast<size_t>(k2) >= m_slotKinds.size())
        return false;
    //Kind must match.
    if (m_slotKinds[k1] != m_slotKinds[k2]) return false;
    auto kind = m_slotKinds[k1];
    if (kind != RTK_Boxed && kind != RTK_Class && kind != RTK_Struct)
        return false;
    if (kind == RTK_Class || kind == RTK_Struct)
        return k1 == k2;  //identity (k1!=k2 already checked above → false)
    //RTK_Boxed: branch on the wrapped type tag (slot[0]).
    int32_t tag1 = m_structHeap[static_cast<size_t>(k1)][0];
    int32_t tag2 = m_structHeap[static_cast<size_t>(k2)][0];
    if (tag1 != tag2) return false;
    int32_t bits1 = m_structHeap[static_cast<size_t>(k1)][kBoxedValueSlot];
    int32_t bits2 = m_structHeap[static_cast<size_t>(k2)][kBoxedValueSlot];
    if (tag1 == RTK_String) {
        //bits are string-pool idxs.
        if (bits1 < 0 || bits1 >= (int32_t)m_stringPool.size()) return false;
        if (bits2 < 0 || bits2 >= (int32_t)m_stringPool.size()) return false;
        return m_stringPool[bits1] == m_stringPool[bits2];
    }
    return bits1 == bits2;  //int / float value bits
}

//Phase 8e-3 fix-up: read this.__handle from callParamBase[0] for a List
//intrinsic. Validates this-heap-idx, upper bound, and handle. Throws uniformly
//on null/stale/uninitialized; the previous "silently no-op for some methods"
//behavior was inconsistent (H1) and made bugs hard to spot.
int32_t VmExecutor::ReadListHandle(uint16_t callParamBase, uint8_t* locals,
    const char* methodName)
{
    int32_t thisHeapIdx;
    std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
    if (thisHeapIdx <= 0)
        throw std::runtime_error(
            std::string("NLang VM: List ") + methodName + " on null instance");
    if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            std::string("NLang VM: List ") + methodName + " on stale reference");
    int32_t handle = m_structHeap[static_cast<size_t>(thisHeapIdx)]
        [kListHandleFieldOffset];
    if (handle <= 0)
        throw std::runtime_error(
            std::string("NLang VM: List ") + methodName +
            " on uninitialized instance");
    return handle;
}

//Helper: read this.__handle from callParamBase[0].
//Returns the 1-based handle. Throws if invalid or closed.
static int32_t ReadStreamHandle(uint16_t callParamBase, uint8_t* locals,
    const std::vector<std::vector<int32_t>>& structHeap,
    const char* label)
{
    int32_t thisHeapIdx;
    std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
    if (thisHeapIdx <= 0 || static_cast<size_t>(thisHeapIdx) >= structHeap.size())
        throw std::runtime_error(std::string("NLang VM: ") + label + " on null reference");
    int32_t handle = structHeap[static_cast<size_t>(thisHeapIdx)][1]; //slot 1 = __handle
    if (handle <= 0)
        throw std::runtime_error("NLang VM: stream handle is invalid or closed");
    return handle;
}

//Phase 9b-pre: collection toString helpers.
//Python-style formatting: [a, b, c] / {k: v}. Strings quoted with repr-style
//escape. Cyclic/nested structures bounded by TOSTRING_DEPTH_LIMIT (64).
//Throws std::runtime_error on depth overflow; caught by main()'s catch and
//surfaced as exit(1) like other VM errors.

std::string VmExecutor::QuoteString(const std::string& s) const {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

std::string VmExecutor::FormatHeapValue(int32_t heapIdx, int depth) {
    if (depth > static_cast<int>(TOSTRING_DEPTH_LIMIT))
        throw std::runtime_error(
            "NLang VM: toString depth limit exceeded");
    if (heapIdx <= 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        return "<null>";
    uint8_t kind = m_slotKinds[static_cast<size_t>(heapIdx)];
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    switch (kind) {
    case RTK_Boxed: {
        int32_t tag = slot[0];
        int32_t val = slot[1];
        if (tag == RTK_Int32) {
            return std::to_string(val);
        } else if (tag == RTK_Float) {
            float fv;
            std::memcpy(&fv, &val, sizeof(fv));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", fv);
            return buf;
        } else if (tag == RTK_String) {
            if (val >= 0
                && static_cast<size_t>(val) < m_stringPool.size())
                return QuoteString(m_stringPool[static_cast<size_t>(val)]);
            return "\"\"";
        }
        return "<unknown>";
    }
    case RTK_Class: {
        int32_t classIdx = slot[0];
        if (classIdx == m_listClassIdx) {
            int32_t handle = slot[kListHandleFieldOffset];
            return FormatList(handle, depth + 1);
        }
        if (classIdx == m_dictClassIdx) {
            int32_t handle = slot[kListHandleFieldOffset];
            return FormatDict(handle, depth + 1);
        }
        return InvokeVirtualToString(heapIdx);
    }
    case RTK_Struct:
        return "<struct>";
    case RTK_Array:
        return FormatArray(heapIdx, depth + 1);
    default:
        return "<unknown>";
    }
}

std::string VmExecutor::FormatArray(int32_t heapIdx, int depth) {
    if (depth > static_cast<int>(TOSTRING_DEPTH_LIMIT))
        throw std::runtime_error(
            "NLang VM: toString depth limit exceeded");
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    uint16_t arrayTypeIdx = static_cast<uint16_t>(slot[1]);
    int32_t length = slot[2];
    uint8_t elemKind = RTK_Int32;
    if (arrayTypeIdx < m_currModule->arrayTypes.size())
        elemKind = m_currModule->arrayTypes[arrayTypeIdx].elemKind;
    if (length <= 0) return "[]";
    std::string result = "[";
    for (int32_t i = 0; i < length; ++i) {
        if (i > 0) result += ", ";
        int32_t elemVal = slot[3 + static_cast<size_t>(i)];
        switch (elemKind) {
        case RTK_Int32:
            result += std::to_string(elemVal);
            break;
        case RTK_Float: {
            float fv;
            std::memcpy(&fv, &elemVal, sizeof(fv));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", fv);
            result += buf;
            break;
        }
        case RTK_String:
            if (elemVal >= 0
                && static_cast<size_t>(elemVal) < m_stringPool.size())
                result += QuoteString(m_stringPool[static_cast<size_t>(elemVal)]);
            else
                result += "\"\"";
            break;
        case RTK_Struct:
            result += "<struct>";
            break;
        case RTK_Class:
        case RTK_Boxed:
        case RTK_Array:
            result += FormatHeapValue(elemVal, depth);
            break;
        default:
            result += "<unknown>";
            break;
        }
    }
    result += "]";
    return result;
}

std::string VmExecutor::FormatList(int32_t handle, int depth) {
    if (depth > static_cast<int>(TOSTRING_DEPTH_LIMIT))
        throw std::runtime_error(
            "NLang VM: toString depth limit exceeded");
    if (handle <= 0 || static_cast<size_t>(handle) > m_listStore.size())
        return "[]";
    const auto& elems =
        m_listStore[static_cast<size_t>(handle) - 1].elements;
    if (elems.empty()) return "[]";
    std::string result = "[";
    for (size_t i = 0; i < elems.size(); ++i) {
        if (i > 0) result += ", ";
        result += FormatHeapValue(elems[i], depth);
    }
    result += "]";
    return result;
}

std::string VmExecutor::FormatDict(int32_t handle, int depth) {
    if (depth > static_cast<int>(TOSTRING_DEPTH_LIMIT))
        throw std::runtime_error(
            "NLang VM: toString depth limit exceeded");
    if (handle <= 0 || static_cast<size_t>(handle) > m_dictStore.size())
        return "{}";
    const auto& entries =
        m_dictStore[static_cast<size_t>(handle) - 1].entries;
    if (entries.empty()) return "{}";
    std::string result = "{";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) result += ", ";
        result += FormatHeapValue(entries[i].first, depth);
        result += ": ";
        result += FormatHeapValue(entries[i].second, depth);
    }
    result += "}";
    return result;
}

std::string VmExecutor::InvokeVirtualToString(int32_t thisHeapIdx) {
    //Mirror OP_CallMethod's vtable walk: search class hierarchy for
    //a method named "toString". If found, call it; if the resolved
    //function is an intrinsic (Object.toString default or user override
    //on List/Dict), dispatch via ExecuteIntrinsic. If not found, fall
    //back to "<ClassName>" placeholder (shouldn't happen — every class
    //inherits Object.toString).
    if (thisHeapIdx <= 0
        || static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
        return "<null>";
    int32_t classIdx = m_structHeap[static_cast<size_t>(thisHeapIdx)][0];
    if (classIdx < 0
        || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
        return "<unknown>";
    int funcIndex = -1;
    int searchClassIdx = classIdx;
    while (searchClassIdx >= 0
        && searchClassIdx < static_cast<int>(m_currModule->classes.size())) {
        const auto& cc =
            m_currModule->classes[static_cast<size_t>(searchClassIdx)];
        for (uint16_t idx : cc.methodIndices) {
            if (idx < m_currModule->functions.size()
                && m_currModule->functions[idx].name == "toString") {
                funcIndex = static_cast<int>(idx);
                break;
            }
        }
        if (funcIndex >= 0) break;
        searchClassIdx = cc.superClassIdx;
    }
    if (funcIndex < 0)
        return std::string("<") +
            m_currModule->classes[static_cast<size_t>(classIdx)].name + ">";
    const CompiledFunction& callee =
        m_currModule->functions[static_cast<size_t>(funcIndex)];
    //Synthetic 4-byte locals frame: just thisHeapIdx at offset 0.
    alignas(int32_t) uint8_t paramFrame[4] = {0};
    std::memcpy(paramFrame, &thisHeapIdx, sizeof(thisHeapIdx));
    alignas(int32_t) uint8_t resultBuf[4] = {0};
    if (callee.intrinsicId != INTR_None) {
        ExecuteIntrinsic(callee.intrinsicId, 0, paramFrame, resultBuf);
    } else {
        std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
        uint16_t paramBytes = callee.paramCount * sizeof(int32_t);
        if (paramBytes > 0 && paramBytes <= callee.localsSize)
            std::memcpy(calleeLocals.data(), paramFrame, paramBytes);
        ExecuteFunction(callee, resultBuf, calleeLocals.data());
    }
    int32_t strIdx;
    std::memcpy(&strIdx, resultBuf, sizeof(strIdx));
    if (strIdx >= 0
        && static_cast<size_t>(strIdx) < m_stringPool.size())
        return m_stringPool[static_cast<size_t>(strIdx)];
    return "";
}

void VmExecutor::ExecuteIntrinsic(uint16_t intrinsicId, uint16_t callParamBase,
    uint8_t* locals, uint8_t* pResult)
{
    //ByteStream intrinsics (0-14): 0-10 primitives, 11-12 struct, 13-14 object.
    if (intrinsicId <= INTR_BS_ReadObject) {
        switch (intrinsicId) {
        case INTR_BS_Ctor: {
            //this is at callParamBase[0] (heapIdx). Allocate handle, store in __handle.
            int32_t thisHeapIdx;
            std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
            int32_t handle = AllocByteStreamHandle();
            m_structHeap[static_cast<size_t>(thisHeapIdx)][1] = handle;
            break;
        }
        case INTR_BS_WriteInt: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeInt");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t val;
            std::memcpy(&val, locals + callParamBase + VALUE_SIZE, sizeof(val));
            uint8_t bytes[4];
            std::memcpy(bytes, &val, 4);
            st->buf.insert(st->buf.end(), bytes, bytes + 4);
            st->pos += 4;
            break;
        }
        case INTR_BS_ReadInt: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readInt");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (st->pos + 4 > st->buf.size())
                throw std::runtime_error("NLang VM: ReadInt past end of stream");
            int32_t val;
            std::memcpy(&val, st->buf.data() + st->pos, 4);
            st->pos += 4;
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_BS_WriteFloat: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeFloat");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            float val;
            std::memcpy(&val, locals + callParamBase + VALUE_SIZE, sizeof(val));
            uint8_t bytes[4];
            std::memcpy(bytes, &val, 4);
            st->buf.insert(st->buf.end(), bytes, bytes + 4);
            st->pos += 4;
            break;
        }
        case INTR_BS_ReadFloat: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readFloat");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (st->pos + 4 > st->buf.size())
                throw std::runtime_error("NLang VM: ReadFloat past end of stream");
            float val;
            std::memcpy(&val, st->buf.data() + st->pos, 4);
            st->pos += 4;
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_BS_WriteString: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeString");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t strIdx;
            std::memcpy(&strIdx, locals + callParamBase + VALUE_SIZE, sizeof(strIdx));
            const std::string& s = (strIdx >= 0 && static_cast<size_t>(strIdx) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(strIdx)] : "";
            int32_t len = static_cast<int32_t>(s.size());
            uint8_t lenBytes[4];
            std::memcpy(lenBytes, &len, 4);
            st->buf.insert(st->buf.end(), lenBytes, lenBytes + 4);
            st->buf.insert(st->buf.end(), reinterpret_cast<const uint8_t*>(s.data()),
                           reinterpret_cast<const uint8_t*>(s.data()) + s.size());
            st->pos += 4 + static_cast<size_t>(len);
            break;
        }
        case INTR_BS_ReadString: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readString");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (st->pos + 4 > st->buf.size())
                throw std::runtime_error("NLang VM: ReadString length prefix past end of stream");
            int32_t len;
            std::memcpy(&len, st->buf.data() + st->pos, 4);
            st->pos += 4;
            if (len < 0)
                throw std::runtime_error("NLang VM: ReadString length negative (" + std::to_string(len) + ")");
            if (static_cast<size_t>(len) > MAX_STRING_LENGTH)
                throw std::runtime_error(
                    "NLang VM: ReadString length exceeds cap (" + std::to_string(len)
                    + " > " + std::to_string(MAX_STRING_LENGTH) + ")");
            if (st->pos + static_cast<size_t>(len) > st->buf.size())
                throw std::runtime_error("NLang VM: ReadString bytes past end of stream");
            std::string s(reinterpret_cast<const char*>(st->buf.data() + st->pos),
                          static_cast<size_t>(len));
            st->pos += static_cast<size_t>(len);
            int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(std::move(s));
            std::memcpy(pResult, &newIdx, sizeof(newIdx));
            break;
        }
        case INTR_BS_Length: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "length");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t len = static_cast<int32_t>(st->buf.size());
            std::memcpy(pResult, &len, sizeof(len));
            break;
        }
        case INTR_BS_Position: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "position");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t pos = static_cast<int32_t>(st->pos);
            std::memcpy(pResult, &pos, sizeof(pos));
            break;
        }
        case INTR_BS_Reset: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "reset");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            st->pos = 0;
            ClearObjIdState(*st);
            break;
        }
        case INTR_BS_WriteStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeStruct");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t structHeapIdx;
            std::memcpy(&structHeapIdx, locals + callParamBase + VALUE_SIZE,
                sizeof(structHeapIdx));
            if (structHeapIdx <= 0
                || static_cast<size_t>(structHeapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: WriteStruct on null struct");
            uint16_t structIdx = m_slotStructIdx[static_cast<size_t>(structHeapIdx)];
            size_t sizeBefore = st->buf.size();
            //Writer lambda: append bytes to the ByteStream's buffer.
            SerializeStructFields(structHeapIdx, structIdx,
                [&](const uint8_t* p, size_t n) {
                    st->buf.insert(st->buf.end(), p, p + n);
                }, *st);
            st->pos += st->buf.size() - sizeBefore;
            break;
        }
        case INTR_BS_ReadStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readStruct");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t typeNameIdx;
            std::memcpy(&typeNameIdx, locals + callParamBase + VALUE_SIZE,
                sizeof(typeNameIdx));
            const std::string& typeName = (typeNameIdx >= 0
                && static_cast<size_t>(typeNameIdx) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(typeNameIdx)] : "";
            int sIdx = m_currModule->FindStruct(typeName);
            if (sIdx < 0)
                throw std::runtime_error(
                    "NLang VM: ReadStruct type not found: " + typeName);
            uint16_t structIdx = static_cast<uint16_t>(sIdx);
            int32_t rootHeapIdx = AllocStructOnHeap(structIdx);
            //Reader lambda: copy from buffer, advance pos, throw on EOF.
            DeserializeStructFields(rootHeapIdx, structIdx,
                [&](uint8_t* dst, size_t n) {
                    if (st->pos + n > st->buf.size())
                        throw std::runtime_error(
                            "NLang VM: ReadStruct past end of stream");
                    std::memcpy(dst, st->buf.data() + st->pos, n);
                    st->pos += n;
                }, *st);
            std::memcpy(pResult, &rootHeapIdx, sizeof(rootHeapIdx));
            break;
        }
        case INTR_BS_WriteObject: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeObject");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t heapIdx;
            std::memcpy(&heapIdx, locals + callParamBase + VALUE_SIZE,
                sizeof(heapIdx));
            if (heapIdx < 0
                || static_cast<size_t>(heapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: WriteObject invalid heap index");
            size_t sizeBefore = st->buf.size();
            SerializeClassFields(heapIdx,
                [&](const uint8_t* p, size_t n) {
                    st->buf.insert(st->buf.end(), p, p + n);
                }, *st, 0);
            st->pos += st->buf.size() - sizeBefore;
            break;
        }
        case INTR_BS_ReadObject: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readObject");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t typeNameIdx;
            std::memcpy(&typeNameIdx, locals + callParamBase + VALUE_SIZE,
                sizeof(typeNameIdx));
            const std::string& declaredName = (typeNameIdx >= 0
                && static_cast<size_t>(typeNameIdx) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(typeNameIdx)] : "";
            int declaredIdx = m_currModule->FindClass(declaredName);
            if (declaredIdx < 0)
                throw std::runtime_error(
                    "NLang VM: ReadObject class not found: " + declaredName);
            int32_t outHeapIdx = 0;
            DeserializeClassFields(static_cast<uint16_t>(declaredIdx),
                outHeapIdx,
                [&](uint8_t* dst, size_t n) {
                    if (st->pos + n > st->buf.size())
                        throw std::runtime_error(
                            "NLang VM: ReadObject past end of stream");
                    std::memcpy(dst, st->buf.data() + st->pos, n);
                    st->pos += n;
                }, *st, 0);
            std::memcpy(pResult, &outHeapIdx, sizeof(outHeapIdx));
            break;
        }
        case INTR_BS_Close: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "close");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            st->closed = true;
            st->buf.clear();
            st->pos = 0;
            ClearObjIdState(*st);
            //Release handle back to free list.
            size_t idx = static_cast<size_t>(handle) - 1;
            m_byteStreams[idx].reset();
            m_byteStreamFreeList.push_back(handle);
            //Zero the __handle field so subsequent calls fail.
            int32_t thisHeapIdx;
            std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
            m_structHeap[static_cast<size_t>(thisHeapIdx)][1] = 0;
            break;
        }
        }
        return;
    }

    //FileStream intrinsics (20-33): 20-29 primitives, 30-31 struct, 32-33 object.
    if (intrinsicId >= INTR_FS_Ctor && intrinsicId <= INTR_FS_ReadObject) {
        switch (intrinsicId) {
        case INTR_FS_Ctor: {
            //this at callParamBase[0], path string idx at [1], mode string idx at [2].
            int32_t thisHeapIdx;
            std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
            int32_t pathIdx, modeIdx;
            std::memcpy(&pathIdx, locals + callParamBase + VALUE_SIZE, sizeof(pathIdx));
            std::memcpy(&modeIdx, locals + callParamBase + 2 * VALUE_SIZE, sizeof(modeIdx));
            const std::string& path = (pathIdx >= 0 && static_cast<size_t>(pathIdx) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(pathIdx)] : "";
            const std::string& mode = (modeIdx >= 0 && static_cast<size_t>(modeIdx) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(modeIdx)] : "";
            if (mode != "r" && mode != "w" && mode != "a")
                throw std::runtime_error("NLang VM: FileStream mode must be \"r\", \"w\", or \"a\"");
            auto fstate = std::make_unique<FileStreamState>();
            std::ios_base::openmode om = std::ios_base::binary;
            if (mode == "r") { om |= std::ios_base::in; fstate->readable = true; }
            else if (mode == "w") { om |= std::ios_base::out | std::ios_base::trunc; fstate->writable = true; }
            else { om |= std::ios_base::out | std::ios_base::app; fstate->writable = true; }
            auto fs = std::make_unique<std::fstream>();
            fs->open(path, om);
            if (!fs->is_open())
                throw std::runtime_error("NLang VM: FileStream cannot open: " + path);
            fstate->fs = std::move(fs);
            int32_t handle = AllocFileStreamHandle();
            m_fileStreams[static_cast<size_t>(handle) - 1] = std::move(fstate);
            m_structHeap[static_cast<size_t>(thisHeapIdx)][1] = handle;
            break;
        }
        case INTR_FS_WriteInt: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeInt");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            int32_t val;
            std::memcpy(&val, locals + callParamBase + VALUE_SIZE, sizeof(val));
            st->fs->write(reinterpret_cast<const char*>(&val), 4);
            break;
        }
        case INTR_FS_ReadInt: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readInt");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
            int32_t val;
            st->fs->read(reinterpret_cast<char*>(&val), 4);
            if (st->fs->gcount() < 4)
                throw std::runtime_error("NLang VM: ReadInt past end of stream");
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_FS_WriteFloat: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeFloat");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            float val;
            std::memcpy(&val, locals + callParamBase + VALUE_SIZE, sizeof(val));
            st->fs->write(reinterpret_cast<const char*>(&val), 4);
            break;
        }
        case INTR_FS_ReadFloat: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readFloat");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
            float val;
            st->fs->read(reinterpret_cast<char*>(&val), 4);
            if (st->fs->gcount() < 4)
                throw std::runtime_error("NLang VM: ReadFloat past end of stream");
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_FS_WriteString: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeString");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            int32_t strIdx;
            std::memcpy(&strIdx, locals + callParamBase + VALUE_SIZE, sizeof(strIdx));
            const std::string& s = (strIdx >= 0 && static_cast<size_t>(strIdx) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(strIdx)] : "";
            int32_t len = static_cast<int32_t>(s.size());
            st->fs->write(reinterpret_cast<const char*>(&len), 4);
            st->fs->write(s.data(), len);
            break;
        }
        case INTR_FS_ReadString: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readString");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
            int32_t len;
            st->fs->read(reinterpret_cast<char*>(&len), 4);
            if (st->fs->gcount() < 4)
                throw std::runtime_error("NLang VM: ReadString length prefix past end of stream");
            if (len < 0)
                throw std::runtime_error("NLang VM: ReadString length negative (" + std::to_string(len) + ")");
            if (static_cast<size_t>(len) > MAX_STRING_LENGTH)
                throw std::runtime_error(
                    "NLang VM: ReadString length exceeds cap (" + std::to_string(len)
                    + " > " + std::to_string(MAX_STRING_LENGTH) + ")");
            std::string s(static_cast<size_t>(len), '\0');
            st->fs->read(&s[0], len);
            if (st->fs->gcount() < len)
                throw std::runtime_error("NLang VM: ReadString bytes past end of stream");
            int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(std::move(s));
            std::memcpy(pResult, &newIdx, sizeof(newIdx));
            break;
        }
        case INTR_FS_Length: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "length");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            auto cur = st->fs->tellg();
            st->fs->seekg(0, std::ios_base::end);
            auto sz = st->fs->tellg();
            st->fs->seekg(cur);
            int32_t len = static_cast<int32_t>(sz);
            std::memcpy(pResult, &len, sizeof(len));
            break;
        }
        case INTR_FS_Position: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "position");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t pos = static_cast<int32_t>(st->fs->tellg());
            std::memcpy(pResult, &pos, sizeof(pos));
            break;
        }
        case INTR_FS_WriteStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeStruct");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            int32_t structHeapIdx;
            std::memcpy(&structHeapIdx, locals + callParamBase + VALUE_SIZE,
                sizeof(structHeapIdx));
            if (structHeapIdx <= 0
                || static_cast<size_t>(structHeapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: WriteStruct on null struct");
            uint16_t structIdx = m_slotStructIdx[static_cast<size_t>(structHeapIdx)];
            SerializeStructFields(structHeapIdx, structIdx,
                [&](const uint8_t* p, size_t n) {
                    st->fs->write(reinterpret_cast<const char*>(p),
                        static_cast<std::streamsize>(n));
                }, *st);
            break;
        }
        case INTR_FS_ReadStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readStruct");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
            int32_t typeNameIdx;
            std::memcpy(&typeNameIdx, locals + callParamBase + VALUE_SIZE,
                sizeof(typeNameIdx));
            const std::string& typeName = (typeNameIdx >= 0
                && static_cast<size_t>(typeNameIdx) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(typeNameIdx)] : "";
            int sIdx = m_currModule->FindStruct(typeName);
            if (sIdx < 0)
                throw std::runtime_error(
                    "NLang VM: ReadStruct type not found: " + typeName);
            uint16_t structIdx = static_cast<uint16_t>(sIdx);
            int32_t rootHeapIdx = AllocStructOnHeap(structIdx);
            DeserializeStructFields(rootHeapIdx, structIdx,
                [&](uint8_t* dst, size_t n) {
                    st->fs->read(reinterpret_cast<char*>(dst),
                        static_cast<std::streamsize>(n));
                    if (st->fs->gcount() < static_cast<std::streamsize>(n))
                        throw std::runtime_error(
                            "NLang VM: ReadStruct past end of stream");
                }, *st);
            std::memcpy(pResult, &rootHeapIdx, sizeof(rootHeapIdx));
            break;
        }
        case INTR_FS_WriteObject: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeObject");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            int32_t heapIdx;
            std::memcpy(&heapIdx, locals + callParamBase + VALUE_SIZE,
                sizeof(heapIdx));
            if (heapIdx < 0
                || static_cast<size_t>(heapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: WriteObject invalid heap index");
            SerializeClassFields(heapIdx,
                [&](const uint8_t* p, size_t n) {
                    st->fs->write(reinterpret_cast<const char*>(p),
                        static_cast<std::streamsize>(n));
                }, *st, 0);
            break;
        }
        case INTR_FS_ReadObject: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readObject");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
            int32_t typeNameIdx;
            std::memcpy(&typeNameIdx, locals + callParamBase + VALUE_SIZE,
                sizeof(typeNameIdx));
            const std::string& declaredName = (typeNameIdx >= 0
                && static_cast<size_t>(typeNameIdx) < m_stringPool.size())
                ? m_stringPool[static_cast<size_t>(typeNameIdx)] : "";
            int declaredIdx = m_currModule->FindClass(declaredName);
            if (declaredIdx < 0)
                throw std::runtime_error(
                    "NLang VM: ReadObject class not found: " + declaredName);
            int32_t outHeapIdx = 0;
            DeserializeClassFields(static_cast<uint16_t>(declaredIdx),
                outHeapIdx,
                [&](uint8_t* dst, size_t n) {
                    st->fs->read(reinterpret_cast<char*>(dst),
                        static_cast<std::streamsize>(n));
                    if (st->fs->gcount() < static_cast<std::streamsize>(n))
                        throw std::runtime_error(
                            "NLang VM: ReadObject past end of stream");
                }, *st, 0);
            std::memcpy(pResult, &outHeapIdx, sizeof(outHeapIdx));
            break;
        }
        case INTR_FS_Close: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "close");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (st) {
                st->closed = true;
                if (st->fs) st->fs->close();
                ClearObjIdState(*st);
            }
            size_t idx = static_cast<size_t>(handle) - 1;
            m_fileStreams[idx].reset();
            m_fileStreamFreeList.push_back(handle);
            int32_t thisHeapIdx;
            std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
            m_structHeap[static_cast<size_t>(thisHeapIdx)][1] = 0;
            break;
        }
        }
        return;
    }

    //Phase 8e-1: Object protocol intrinsics.
    //Object.Equals(this, other) → 0 or 1 (identity comparison on heap idx).
    //Object.GetHashCode(this) → heap idx as int32 (identity-based hash).
    //Both handle null: two nulls are equal, null has hash 0.
    if (intrinsicId == INTR_Object_Equals) {
        int32_t thisHeapIdx, otherHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        std::memcpy(&otherHeapIdx, locals + callParamBase + VALUE_SIZE,
                    sizeof(otherHeapIdx));
        int32_t result;
        if (thisHeapIdx == 0 && otherHeapIdx == 0)
            result = 1;  //two nulls are equal
        else if (thisHeapIdx == 0 || otherHeapIdx == 0)
            result = 0;  //one null, one non-null
        else
            result = (thisHeapIdx == otherHeapIdx) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    if (intrinsicId == INTR_Object_GetHashCode) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        int32_t result = (thisHeapIdx > 0) ? thisHeapIdx : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }

    //Phase 8e-1: String protocol intrinsics.
    //String.Equals(this, other) → value equality via std::string comparison.
    //String.GetHashCode(this) → std::hash<std::string>.
    //Both treat null/pool-out-of-range as empty string.
    if (intrinsicId == INTR_String_Equals) {
        int32_t thisStrIdx, otherStrIdx;
        std::memcpy(&thisStrIdx, locals + callParamBase, sizeof(thisStrIdx));
        std::memcpy(&otherStrIdx, locals + callParamBase + VALUE_SIZE,
                    sizeof(otherStrIdx));
        const std::string& a = (thisStrIdx >= 0
            && static_cast<size_t>(thisStrIdx) < m_stringPool.size())
            ? m_stringPool[static_cast<size_t>(thisStrIdx)] : "";
        const std::string& b = (otherStrIdx >= 0
            && static_cast<size_t>(otherStrIdx) < m_stringPool.size())
            ? m_stringPool[static_cast<size_t>(otherStrIdx)] : "";
        int32_t result = (a == b) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    if (intrinsicId == INTR_String_GetHashCode) {
        int32_t thisStrIdx;
        std::memcpy(&thisStrIdx, locals + callParamBase, sizeof(thisStrIdx));
        const std::string& s = (thisStrIdx >= 0
            && static_cast<size_t>(thisStrIdx) < m_stringPool.size())
            ? m_stringPool[static_cast<size_t>(thisStrIdx)] : "";
        int32_t hash = static_cast<int32_t>(
            std::hash<std::string>{}(s));
        std::memcpy(pResult, &hash, sizeof(hash));
        return;
    }

    //Phase 8e-9b: Object.toString() default intrinsic.
    //Returns "TypeName@hex(heapIdx)" — Java-compat (lowercase, no padding).
    //Null receiver throws NPE. The hex uses heap idx directly (not user
    //override of getHashCode) — documented divergence from Java.
    if (intrinsicId == INTR_Object_toString) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        if (thisHeapIdx <= 0)
            throw std::runtime_error("NLang VM: NullPointerException");
        if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
            throw std::runtime_error("NLang VM: toString on invalid heap idx");
        int32_t classIdx = m_structHeap[static_cast<size_t>(thisHeapIdx)][0];
        if (classIdx < 0
            || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
            throw std::runtime_error("NLang VM: toString on invalid class idx");
        const std::string& className =
            m_currModule->classes[static_cast<size_t>(classIdx)].name;
        char buf[64];
        //%x yields lowercase no-padding (Java Integer.toHexString-compatible)
        std::snprintf(buf, sizeof(buf), "%s@%x",
                      className.c_str(),
                      static_cast<unsigned>(thisHeapIdx));
        int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
        m_stringPool.push_back(buf);
        std::memcpy(pResult, &newIdx, sizeof(newIdx));
        return;
    }

    //Phase 8e-3: List<T> built-in generic (erasure-style, 9 intrinsics).
    //All elements are heap idxs — boxed primitives or class refs. The same
    //CompiledClass "List" serves all instantiations. T-typed args are boxed
    //by VmBackend before OP_CallMethod; Get() returns boxed and is unboxed
    //by OP_Unbox after.

    //INTR_List_Ctor: allocate a ListSlot, store handle in __handle field.
    if (intrinsicId == INTR_List_Ctor) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        if (thisHeapIdx <= 0)
            throw std::runtime_error("NLang VM: List ctor on null instance");
        if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
            throw std::runtime_error("NLang VM: List ctor on stale reference");
        int32_t handle = AllocListHandle();
        //__handle is field slot kListHandleFieldOffset (slot 0 = classIdx)
        m_structHeap[static_cast<size_t>(thisHeapIdx)]
            [kListHandleFieldOffset] = handle;
        return;
    }
    //INTR_List_Add: push element to list.
    if (intrinsicId == INTR_List_Add) {
        int32_t value;
        std::memcpy(&value, locals + callParamBase + VALUE_SIZE, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "add");
        m_listStore[handle - 1].elements.push_back(value);
        return;
    }
    //INTR_List_Get: return element at index.
    if (intrinsicId == INTR_List_Get) {
        int32_t idx;
        std::memcpy(&idx, locals + callParamBase + VALUE_SIZE, sizeof(idx));
        int32_t handle = ReadListHandle(callParamBase, locals, "get");
        auto& lst = m_listStore[handle - 1];
        if (idx < 0 || static_cast<size_t>(idx) >= lst.elements.size())
            throw std::runtime_error("NLang VM: List index out of bounds");
        int32_t val = lst.elements[idx];
        std::memcpy(pResult, &val, sizeof(val));
        return;
    }
    //INTR_List_Set: replace element at index.
    if (intrinsicId == INTR_List_Set) {
        int32_t idx, value;
        std::memcpy(&idx, locals + callParamBase + VALUE_SIZE, sizeof(idx));
        std::memcpy(&value, locals + callParamBase + 2 * VALUE_SIZE, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "set");
        auto& lst = m_listStore[handle - 1];
        if (idx < 0 || static_cast<size_t>(idx) >= lst.elements.size())
            throw std::runtime_error("NLang VM: List index out of bounds");
        lst.elements[idx] = value;
        return;
    }
    //INTR_List_Length: return elements.size().
    if (intrinsicId == INTR_List_Length) {
        int32_t handle = ReadListHandle(callParamBase, locals, "length");
        int32_t len = static_cast<int32_t>(m_listStore[handle - 1].elements.size());
        std::memcpy(pResult, &len, sizeof(len));
        return;
    }
    //INTR_List_RemoveAt: erase element at index.
    if (intrinsicId == INTR_List_RemoveAt) {
        int32_t idx;
        std::memcpy(&idx, locals + callParamBase + VALUE_SIZE, sizeof(idx));
        int32_t handle = ReadListHandle(callParamBase, locals, "removeAt");
        auto& lst = m_listStore[handle - 1];
        if (idx < 0 || static_cast<size_t>(idx) >= lst.elements.size())
            throw std::runtime_error("NLang VM: List index out of bounds");
        lst.elements.erase(lst.elements.begin() + idx);
        return;
    }
    //INTR_List_IndexOf: linear search; return position or -1.
    //C2 fix: for primitive-T lists, OP_Box wraps `value` before this call,
    //so `value` is a heap idx into a RTK_Boxed slot. Compare slot[1] (value
    //bits). For class-T lists, `value` is the heap idx directly. Decide by
    //peeking at m_slotKinds of the first element (or of `value` itself when
    //the list is empty — caller-side OP_Box still allocated a slot we can
    //inspect).
    if (intrinsicId == INTR_List_IndexOf) {
        int32_t value;
        std::memcpy(&value, locals + callParamBase + VALUE_SIZE, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "indexOf");
        auto& lst = m_listStore[handle - 1];
        int32_t result = -1;
        bool primitiveT = (value > 0
            && static_cast<size_t>(value) < m_slotKinds.size()
            && m_slotKinds[value] == RTK_Boxed);
        int32_t valBits = 0;
        if (primitiveT)
            valBits = m_structHeap[static_cast<size_t>(value)]
                [kBoxedValueSlot];
        for (size_t i = 0; i < lst.elements.size(); ++i) {
            int32_t elem = lst.elements[i];
            bool match = false;
            if (primitiveT) {
                if (elem > 0
                    && static_cast<size_t>(elem) < m_slotKinds.size()
                    && m_slotKinds[elem] == RTK_Boxed
                    && m_structHeap[static_cast<size_t>(elem)]
                        [kBoxedValueSlot] == valBits) {
                    match = true;
                }
            } else {
                if (elem == value) match = true;
            }
            if (match) { result = static_cast<int32_t>(i); break; }
        }
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    //INTR_List_Contains: return 1 if found, 0 otherwise. Same primitive-vs-
    //class branching as IndexOf (C2 fix).
    if (intrinsicId == INTR_List_Contains) {
        int32_t value;
        std::memcpy(&value, locals + callParamBase + VALUE_SIZE, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "contains");
        auto& lst = m_listStore[handle - 1];
        int32_t result = 0;
        bool primitiveT = (value > 0
            && static_cast<size_t>(value) < m_slotKinds.size()
            && m_slotKinds[value] == RTK_Boxed);
        int32_t valBits = 0;
        if (primitiveT)
            valBits = m_structHeap[static_cast<size_t>(value)]
                [kBoxedValueSlot];
        for (size_t i = 0; i < lst.elements.size(); ++i) {
            int32_t elem = lst.elements[i];
            bool match = false;
            if (primitiveT) {
                if (elem > 0
                    && static_cast<size_t>(elem) < m_slotKinds.size()
                    && m_slotKinds[elem] == RTK_Boxed
                    && m_structHeap[static_cast<size_t>(elem)]
                        [kBoxedValueSlot] == valBits) {
                    match = true;
                }
            } else {
                if (elem == value) match = true;
            }
            if (match) { result = 1; break; }
        }
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    //INTR_List_Clear: remove all elements.
    if (intrinsicId == INTR_List_Clear) {
        int32_t handle = ReadListHandle(callParamBase, locals, "clear");
        m_listStore[handle - 1].elements.clear();
        return;
    }

    //Phase 8e-4: Dict<K,V> built-in generic (erasure-style, 7 intrinsics).
    //Keys and values are uniformly heap idxs (boxed primitives via OP_Box at
    //the call site, or class refs directly). Linear-scan lookup with kind-
    //aware equality via DictKeysEqual.

    //INTR_Dict_Ctor: allocate DictSlot, store handle.
    if (intrinsicId == INTR_Dict_Ctor) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        if (thisHeapIdx <= 0)
            throw std::runtime_error("NLang VM: Dict ctor on null instance");
        if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
            throw std::runtime_error("NLang VM: Dict ctor on stale reference");
        int32_t handle = AllocDictHandle();
        m_structHeap[static_cast<size_t>(thisHeapIdx)]
            [kListHandleFieldOffset] = handle;
        return;
    }
    //INTR_Dict_Set: insert-or-replace (linear scan).
    if (intrinsicId == INTR_Dict_Set) {
        int32_t k, v;
        std::memcpy(&k, locals + callParamBase + VALUE_SIZE, sizeof(k));
        std::memcpy(&v, locals + callParamBase + 2 * VALUE_SIZE, sizeof(v));
        int32_t handle = ReadDictHandle(callParamBase, locals, "set");
        auto& entries = m_dictStore[handle - 1].entries;
        for (auto& kv : entries) {
            if (DictKeysEqual(kv.first, k)) { kv.second = v; return; }
        }
        entries.push_back({k, v});
        return;
    }
    //INTR_Dict_Get: lookup; throw on missing key.
    if (intrinsicId == INTR_Dict_Get) {
        int32_t k;
        std::memcpy(&k, locals + callParamBase + VALUE_SIZE, sizeof(k));
        int32_t handle = ReadDictHandle(callParamBase, locals, "get");
        auto& entries = m_dictStore[handle - 1].entries;
        for (auto& kv : entries) {
            if (DictKeysEqual(kv.first, k)) {
                std::memcpy(pResult, &kv.second, sizeof(kv.second));
                return;
            }
        }
        throw std::runtime_error("NLang VM: Dict key not found");
    }
    //INTR_Dict_ContainsKey: 1 if found, 0 otherwise.
    if (intrinsicId == INTR_Dict_ContainsKey) {
        int32_t k;
        std::memcpy(&k, locals + callParamBase + VALUE_SIZE, sizeof(k));
        int32_t handle = ReadDictHandle(callParamBase, locals, "containsKey");
        auto& entries = m_dictStore[handle - 1].entries;
        int32_t result = 0;
        for (auto& kv : entries) {
            if (DictKeysEqual(kv.first, k)) { result = 1; break; }
        }
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    //INTR_Dict_Remove: 1 if removed, 0 if not found.
    if (intrinsicId == INTR_Dict_Remove) {
        int32_t k;
        std::memcpy(&k, locals + callParamBase + VALUE_SIZE, sizeof(k));
        int32_t handle = ReadDictHandle(callParamBase, locals, "remove");
        auto& entries = m_dictStore[handle - 1].entries;
        int32_t result = 0;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (DictKeysEqual(it->first, k)) {
                entries.erase(it);
                result = 1;
                break;
            }
        }
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    //INTR_Dict_Clear: drop all entries.
    if (intrinsicId == INTR_Dict_Clear) {
        int32_t handle = ReadDictHandle(callParamBase, locals, "clear");
        m_dictStore[handle - 1].entries.clear();
        return;
    }
    //INTR_Dict_Count: entry count.
    if (intrinsicId == INTR_Dict_Count) {
        int32_t handle = ReadDictHandle(callParamBase, locals, "count");
        int32_t n = static_cast<int32_t>(m_dictStore[handle - 1].entries.size());
        std::memcpy(pResult, &n, sizeof(n));
        return;
    }
    //INTR_Dict_Keys (Phase 8e-5): allocate a fresh List<K> heap instance and
    //populate it with every dict entry's key (entries[i].first). The result
    //is a heap idx the caller treats as List<K>; element type K is known to
    //codegen (set when the resolver synthesized the List<K> return type), so
    //any subsequent Get(i) on the result will unbox correctly for primitive K.
    if (intrinsicId == INTR_Dict_Keys) {
        int32_t dictHandle = ReadDictHandle(callParamBase, locals, "keys");
        auto& src = m_dictStore[dictHandle - 1].entries;

        //Allocate the List<K> class instance on the heap + a fresh List handle.
        if (m_listClassIdx < 0)
            throw std::runtime_error("NLang VM: List class not registered");
        int32_t listHeapIdx = AllocClassOnHeap(
            static_cast<uint16_t>(m_listClassIdx));
        int32_t listHandle  = AllocListHandle();
        m_structHeap[static_cast<size_t>(listHeapIdx)]
            [kListHandleFieldOffset] = listHandle;

        //Populate elements from dict keys.
        auto& dst = m_listStore[listHandle - 1].elements;
        dst.reserve(src.size());
        for (auto& kv : src)
            dst.push_back(kv.first);

        std::memcpy(pResult, &listHeapIdx, sizeof(listHeapIdx));
        m_gcPending = true;
        return;
    }

    //INTR_List_toString (Phase 9b-pre): format the list's elements as
    //"[e1, e2, ...]" via FormatList. Push result to m_stringPool, write
    //string idx to pResult. Strings inside are quoted via QuoteString.
    if (intrinsicId == INTR_List_toString) {
        int32_t handle = ReadListHandle(callParamBase, locals, "toString");
        std::string s = FormatList(handle, 0);
        int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
        m_stringPool.push_back(std::move(s));
        std::memcpy(pResult, &newIdx, sizeof(newIdx));
        return;
    }
    //INTR_Dict_toString (Phase 9b-pre): format the dict's entries as
    //"{k1: v1, k2: v2, ...}" via FormatDict.
    if (intrinsicId == INTR_Dict_toString) {
        int32_t handle = ReadDictHandle(callParamBase, locals, "toString");
        std::string s = FormatDict(handle, 0);
        int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
        m_stringPool.push_back(std::move(s));
        std::memcpy(pResult, &newIdx, sizeof(newIdx));
        return;
    }

    throw std::runtime_error("NLang VM: unknown intrinsic id " + std::to_string(intrinsicId));
}

}
