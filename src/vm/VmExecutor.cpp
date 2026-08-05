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
void VmExecutor::DeserializeClassFields(uint16_t expectedClassIdx,
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
    std::string className(static_cast<size_t>(nameLen), '\0');
    if (nameLen > 0)
        read(reinterpret_cast<uint8_t*>(&className[0]), nameLen);

    int classIdx = m_currModule->FindClass(className);
    if (classIdx < 0)
        throw std::runtime_error(
            "NLang VM: ReadStruct class not found: " + className);
    if (static_cast<uint16_t>(classIdx) != expectedClassIdx) {
        const std::string& expectedName =
            m_currModule->classes[expectedClassIdx].name;
        throw std::runtime_error(
            "NLang VM: ReadStruct class type mismatch: expected "
            + expectedName + ", got " + className);
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

void VmExecutor::ExecuteIntrinsic(uint16_t intrinsicId, uint16_t callParamBase,
    uint8_t* locals, uint8_t* pResult)
{
    //ByteStream intrinsics (0-12): 0-10 primitives, 11-12 struct.
    if (intrinsicId <= INTR_BS_ReadStruct) {
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
                m_structHeap, "WriteInt");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t val;
            std::memcpy(&val, locals + callParamBase + VALUE_SIZE, sizeof(val));
            uint8_t bytes[4];
            std::memcpy(bytes, &val, 4);
            st->buf.insert(st->buf.end(), bytes, bytes + 4);
            break;
        }
        case INTR_BS_ReadInt: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "ReadInt");
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
                m_structHeap, "WriteFloat");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            float val;
            std::memcpy(&val, locals + callParamBase + VALUE_SIZE, sizeof(val));
            uint8_t bytes[4];
            std::memcpy(bytes, &val, 4);
            st->buf.insert(st->buf.end(), bytes, bytes + 4);
            break;
        }
        case INTR_BS_ReadFloat: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "ReadFloat");
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
                m_structHeap, "WriteString");
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
            break;
        }
        case INTR_BS_ReadString: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "ReadString");
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
                m_structHeap, "Length");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t len = static_cast<int32_t>(st->buf.size());
            std::memcpy(pResult, &len, sizeof(len));
            break;
        }
        case INTR_BS_Position: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "Position");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t pos = static_cast<int32_t>(st->pos);
            std::memcpy(pResult, &pos, sizeof(pos));
            break;
        }
        case INTR_BS_Reset: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "Reset");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            st->pos = 0;
            ClearObjIdState(*st);
            break;
        }
        case INTR_BS_WriteStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "WriteStruct");
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
            //Writer lambda: append bytes to the ByteStream's buffer.
            SerializeStructFields(structHeapIdx, structIdx,
                [&](const uint8_t* p, size_t n) {
                    st->buf.insert(st->buf.end(), p, p + n);
                }, *st);
            break;
        }
        case INTR_BS_ReadStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "ReadStruct");
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
        case INTR_BS_Close: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "Close");
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

    //FileStream intrinsics (20-31): 20-29 primitives, 30-31 struct.
    if (intrinsicId >= INTR_FS_Ctor && intrinsicId <= INTR_FS_ReadStruct) {
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
                m_structHeap, "WriteInt");
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
                m_structHeap, "ReadInt");
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
                m_structHeap, "WriteFloat");
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
                m_structHeap, "ReadFloat");
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
                m_structHeap, "WriteString");
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
                m_structHeap, "ReadString");
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
                m_structHeap, "Length");
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
                m_structHeap, "Position");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t pos = static_cast<int32_t>(st->fs->tellg());
            std::memcpy(pResult, &pos, sizeof(pos));
            break;
        }
        case INTR_FS_WriteStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "WriteStruct");
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
                m_structHeap, "ReadStruct");
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
        case INTR_FS_Close: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "Close");
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

    throw std::runtime_error("NLang VM: unknown intrinsic id " + std::to_string(intrinsicId));
}

}
