/*---
    VmExecutorOpsCalls.cpp — 函数调用/委托/函数句柄操作码与委托调用引擎
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

void VmExecutor::OpCallFunc(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult) {
    uint16_t funcIndex = reader.ReadUint16();
    uint16_t callParamBase = reader.ReadUint16();
    if (funcIndex >= m_currModule->functions.size())
        throw std::runtime_error("NLang VM: invalid function index");
    const CompiledFunction& callee = m_currModule->functions[funcIndex];
    //Phase 9f: native declaration — dispatch to the host-registered
    //table instead of interpreting bytecode. The native reads args
    //directly from the caller's callParamBase cells (same ABI as
    //intrinsics) and writes its return into pResult.
    if (callee.isNative) {
        CallNative(callee, callParamBase, locals, pResult);
        return;
    }
    std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
    uint16_t paramBytes = callee.paramCount * kFrameSlotBytes;
    if (paramBytes > 0 && paramBytes <= callee.localsSize)
        std::memcpy(calleeLocals.data(), locals + callParamBase, paramBytes);
    ExecuteFunction(callee, pResult, calleeLocals.data());
}

void VmExecutor::OpMakeFunc(BytecodeReader& reader, uint8_t* pResult) {
    //Static-bound handle to a free function: {funcIdx, this=0,
    //form=0}. Allocation always happens (no interning) so two
    //references to one function are distinct-but-equal records.
    uint16_t funcIdx = reader.ReadUint16();
    if (funcIdx >= m_currModule->functions.size())
        throw std::runtime_error("NLang VM: invalid function index in MakeFunc");
    int32_t heapIdx = AllocFuncRecord(
        static_cast<int32_t>(funcIdx), 0, kFuncFormStatic);
    std::memcpy(pResult, &heapIdx, sizeof(heapIdx));
}

void VmExecutor::OpCallDelegate(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult) {
    uint16_t calleeLocal = reader.ReadUint16();
    uint16_t callParamBase = reader.ReadUint16();
    int32_t handleIdx;
    std::memcpy(&handleIdx, locals + calleeLocal, sizeof(handleIdx));
    if (handleIdx <= 0
        || static_cast<size_t>(handleIdx) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(handleIdx)] != RTK_Func)
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null function value in CallDelegate");
    ExecuteDelegateCall(m_structHeap[static_cast<size_t>(handleIdx)],
                        callParamBase, locals, pResult, 0);
}

void VmExecutor::OpCallDelegateOut(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult) {
    uint16_t calleeLocal = reader.ReadUint16();
    uint16_t callParamBase = reader.ReadUint16();
    uint32_t outMask = reader.ReadUint32();
    int32_t handleIdx;
    std::memcpy(&handleIdx, locals + calleeLocal, sizeof(handleIdx));
    if (handleIdx <= 0
        || static_cast<size_t>(handleIdx) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(handleIdx)] != RTK_Func)
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null function value in CallDelegate");
    ExecuteDelegateCall(m_structHeap[static_cast<size_t>(handleIdx)],
                        callParamBase, locals, pResult, outMask);
}

void VmExecutor::OpMakeBoundFunc(BytecodeReader& reader, uint8_t* pResult) {
    //Bound non-virtual method: receiver was emitted to the
    //result slot first (receiver-first emission shape), so this
    //reads pResult BEFORE writing the handle. A null receiver
    //throws at BIND time — a handle materialized with this==0
    //would dispatch as a free function and silently misframe.
    uint16_t funcIdx = reader.ReadUint16();
    int32_t thisIdx;
    std::memcpy(&thisIdx, pResult, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null receiver in method reference");
    if (funcIdx >= m_currModule->functions.size())
        throw std::runtime_error(
            "NLang VM: invalid function index in MakeBoundFunc");
    int32_t heapIdx = AllocFuncRecord(
        static_cast<int32_t>(funcIdx), thisIdx, kFuncFormStatic);
    std::memcpy(pResult, &heapIdx, sizeof(heapIdx));
}

void VmExecutor::OpMakeVFunc(BytecodeReader& reader, uint8_t* pResult) {
    //Virtual-dispatch handle: stores the method NAME (string
    //pool index) instead of a function index; dispatch resolves
    //the override chain on the runtime class at call time. Same
    //bind-time null-receiver guard as MakeBoundFunc.
    uint16_t nameIdx = reader.ReadUint16();
    int32_t thisIdx;
    std::memcpy(&thisIdx, pResult, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null receiver in method reference");
    if (nameIdx >= m_currModule->stringConstants.size())
        throw std::runtime_error(
            "NLang VM: invalid string index in MakeVFunc");
    int32_t heapIdx = AllocFuncRecord(
        static_cast<int32_t>(nameIdx), thisIdx, kFuncFormVirtual);
    std::memcpy(pResult, &heapIdx, sizeof(heapIdx));
}

void VmExecutor::OpCallFuncOut(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult) {
    uint16_t funcIndex = reader.ReadUint16();
    uint16_t callParamBase = reader.ReadUint16();
    uint32_t outMask = reader.ReadUint32();
    if (funcIndex >= m_currModule->functions.size())
        throw std::runtime_error("NLang VM: invalid function index");
    const CompiledFunction& callee = m_currModule->functions[funcIndex];
    //Phase 9f: out writeback needs a callee frame to read from;
    //natives have none. Reject loudly rather than silently
    //copying back the unmodified argument cells.
    if (callee.isNative)
        throw std::runtime_error(
            "NLang VM: native function does not support out parameters: "
            + callee.name);
    //Phase 10 audit H1: intrinsics execute without a callee frame
    //too — the writeback would silently copy nothing. Unreachable
    //today (out args only bind on user functions); guard anyway.
    if (callee.intrinsicId != INTR_None)
        throw std::runtime_error(
            "NLang VM: intrinsic function does not support out parameters: "
            + callee.name);
    std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
    uint16_t paramBytes = callee.paramCount * kFrameSlotBytes;
    if (paramBytes > 0 && paramBytes <= callee.localsSize)
        std::memcpy(calleeLocals.data(), locals + callParamBase, paramBytes);
    ExecuteFunction(callee, pResult, calleeLocals.data());
    for (uint32_t slot = 0; slot < 32; ++slot) {
        if (!(outMask & (1u << slot)))
            continue;
        uint16_t off = static_cast<uint16_t>(slot * kFrameSlotBytes);
        if (off + kFrameSlotBytes > callee.localsSize)
            throw std::runtime_error("NLang VM: out parameter slot out of bounds");
        std::memcpy(locals + callParamBase + off,
                    calleeLocals.data() + off, kFrameSlotBytes);
    }
}

void VmExecutor::OpCallMethodDirect(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult) {
    uint16_t funcIndex = reader.ReadUint16();
    uint16_t callParamBase = reader.ReadUint16();
    if (funcIndex >= m_currModule->functions.size())
        throw std::runtime_error("NLang VM: invalid function index in CallMethodDirect");
    const CompiledFunction& callee = m_currModule->functions[funcIndex];
    if (callee.intrinsicId != INTR_None) {
        ExecuteIntrinsic(callee.intrinsicId, callParamBase, locals, pResult);
        return;
    }
    //Phase 9f: native method — same table dispatch, `this` rides
    //at args[0] per the bytecode calling convention.
    if (callee.isNative) {
        CallNative(callee, callParamBase, locals, pResult);
        return;
    }
    std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
    uint16_t paramBytes = callee.paramCount * kFrameSlotBytes;
    if (paramBytes > 0 && paramBytes <= callee.localsSize)
        std::memcpy(calleeLocals.data(), locals + callParamBase, paramBytes);
    ExecuteFunction(callee, pResult, calleeLocals.data());
}

void VmExecutor::OpCallMethodDirectOut(BytecodeReader& reader, uint8_t* locals,
    uint8_t* pResult) {
    uint16_t funcIndex = reader.ReadUint16();
    uint16_t callParamBase = reader.ReadUint16();
    uint32_t outMask = reader.ReadUint32();
    if (funcIndex >= m_currModule->functions.size())
        throw std::runtime_error("NLang VM: invalid function index in CallMethodDirect");
    const CompiledFunction& callee = m_currModule->functions[funcIndex];
    //Phase 10 audit H1: intrinsics execute without a callee frame,
    //so the outMask writeback would be silently dropped (the old
    //branch just called ExecuteIntrinsic and broke). Unreachable
    //today (out args only bind on user functions); reject loudly,
    //matching the native branch below.
    if (callee.intrinsicId != INTR_None)
        throw std::runtime_error(
            "NLang VM: intrinsic function does not support out parameters: "
            + callee.name);
    //Phase 9f: out writeback needs a callee frame; natives have
    //none (see OP_CallFuncOut). Reject loudly, not silently.
    if (callee.isNative)
        throw std::runtime_error(
            "NLang VM: native function does not support out parameters: "
            + callee.name);
    std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
    uint16_t paramBytes = callee.paramCount * kFrameSlotBytes;
    if (paramBytes > 0 && paramBytes <= callee.localsSize)
        std::memcpy(calleeLocals.data(), locals + callParamBase, paramBytes);
    ExecuteFunction(callee, pResult, calleeLocals.data());
    for (uint32_t slot = 0; slot < 32; ++slot) {
        if (!(outMask & (1u << slot)))
            continue;
        uint16_t off = static_cast<uint16_t>(slot * kFrameSlotBytes);
        if (off + kFrameSlotBytes > callee.localsSize)
            throw std::runtime_error("NLang VM: out parameter slot out of bounds");
        std::memcpy(locals + callParamBase + off,
                    calleeLocals.data() + off, kFrameSlotBytes);
    }
}

void VmExecutor::OpCallMethod(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult) {
    //Virtual method dispatch — name-based lookup.
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
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in CallMethod");
    //Read classIdx from the receiver's header. Boxed receivers
    //dispatch on Object — their header slot holds a type tag
    //(see ReceiverClassIndex).
    int32_t classIdx = ReceiverClassIndex(thisHeapIdx);
    if (classIdx < 0 || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
        throw std::runtime_error("NLang VM: invalid class index in object header");
    //Walk class hierarchy to find the method by name (helper
    //shared with virtual-dispatch delegate handles, Phase 13).
    int funcIndex = FindMethodByName(classIdx, methodName);
    if (funcIndex < 0)
        throw std::runtime_error("NLang VM: method not found: " + methodName);
    const CompiledFunction& callee = m_currModule->functions[static_cast<size_t>(funcIndex)];
    if (callee.intrinsicId != INTR_None) {
        ExecuteIntrinsic(callee.intrinsicId, callParamBase, locals, pResult);
        return;
    }
    //Phase 9f: native method found via virtual dispatch — same
    //table dispatch, `this` rides at args[0].
    if (callee.isNative) {
        CallNative(callee, callParamBase, locals, pResult);
        return;
    }
    std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
    uint16_t paramBytes = callee.paramCount * kFrameSlotBytes;
    if (paramBytes > 0 && paramBytes <= callee.localsSize)
        std::memcpy(calleeLocals.data(), locals + callParamBase, paramBytes);
    ExecuteFunction(callee, pResult, calleeLocals.data());
}

void VmExecutor::OpCallIntrinsic(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult) {
    //Emitted by VmBackend for string.getHashCode() / string.equals()
    //(strings are primitives without a class, so the call can't go
    //through OP_CallMethod's callee.intrinsicId path). Dispatch
    //reuses the same ExecuteIntrinsic used by the method path.
    uint16_t intrinsicId = reader.ReadUint16();
    uint16_t callParamBase = reader.ReadUint16();
    ExecuteIntrinsic(intrinsicId, callParamBase, locals, pResult);
}

//Phase 13: allocate one function-handle heap record. Mirrors
//AllocBoxedValue's freelist discipline; 3 slots and RTK_Func kind.
int32_t VmExecutor::AllocFuncRecord(int32_t target, int32_t thisIdx,
                                    int32_t form) {
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(3, 0);
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.emplace_back(3, 0);
        m_slotKinds.push_back(0);
        m_slotStructIdx.push_back(0);
    }
    m_structHeap[static_cast<size_t>(heapIdx)][0] = target;
    m_structHeap[static_cast<size_t>(heapIdx)][1] = thisIdx;
    m_structHeap[static_cast<size_t>(heapIdx)][2] = form;
    m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Func;
    m_slotStructIdx[static_cast<size_t>(heapIdx)] = 0;
    m_gcPending = true;
    return heapIdx;
}

//Phase 13 Step 2: name-based method resolution on the runtime class.
//Walks methodIndices then up the superClassIdx chain — the lookup
//OP_CallMethod has always performed, extracted so virtual-dispatch
//handles share it. Returns a functions[] index or -1.
int VmExecutor::FindMethodByName(int classIdx,
    const std::string& methodName) const {
    int searchClassIdx = classIdx;
    while (searchClassIdx >= 0
        && searchClassIdx < static_cast<int>(m_currModule->classes.size())) {
        const auto& cc
            = m_currModule->classes[static_cast<size_t>(searchClassIdx)];
        for (uint16_t idx : cc.methodIndices) {
            //Bare-name method dispatch (D8): method keys never carry a
            //package prefix, so the receiver alone identifies the table.
            if (idx < m_currModule->functions.size()
                && m_currModule->functions[idx].name == methodName)
                return static_cast<int>(idx);
        }
        searchClassIdx = cc.superClassIdx;
    }
    return -1;
}

//Delegate-target resolution half of ExecuteDelegateCall. Virtual
//handles resolve by name on the receiver's runtime class (override
//chain); static handles are a range-checked functions[] index.
int VmExecutor::ResolveDelegateTarget(const std::vector<int32_t>& handle) {
    int32_t thisIdx = handle[1];
    if (handle[2] == kFuncFormVirtual) {
        //Virtual-dispatch handle: resolve the override chain by name on
        //the receiver's runtime class.
        if (thisIdx <= 0
            || static_cast<size_t>(thisIdx) >= m_structHeap.size())
            RaiseNlangException(m_nullPtrExcClassIdx,
                                "NLang VM: null receiver in virtual delegate");
        int32_t nameIdx = handle[0];
        if (nameIdx < 0
            || static_cast<size_t>(nameIdx) >= m_currModule->stringConstants.size())
            throw std::runtime_error(
                "NLang VM: invalid string index in virtual delegate handle");
        const std::string& methodName
            = m_currModule->stringConstants[static_cast<size_t>(nameIdx)];
        //ReceiverClassIndex: a boxed receiver's header slot holds a
        //type tag, not a class index — same dispatch hazard as
        //OP_CallMethod and InvokeVirtualToString.
        int32_t classIdx = ReceiverClassIndex(thisIdx);
        if (classIdx < 0
            || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
            throw std::runtime_error(
                "NLang VM: invalid class index in object header");
        int funcIndex = FindMethodByName(classIdx, methodName);
        if (funcIndex < 0)
            throw std::runtime_error(
                "NLang VM: method not found: " + methodName);
        return funcIndex;
    }
    int funcIndex = handle[0];
    if (funcIndex < 0
        || static_cast<size_t>(funcIndex) >= m_currModule->functions.size())
        throw std::runtime_error(
            "NLang VM: invalid function index in delegate handle");
    return funcIndex;
}

//Free-function ABI — identical to OP_CallFunc: natives read their args
//straight from the caller's callParamBase cells (out parameters are
//impossible without a callee frame, so a nonzero mask is rejected).
void VmExecutor::CallDelegateFree(const CompiledFunction& callee,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
    uint32_t outMask) {
    if (callee.isNative) {
        if (outMask != 0)
            throw std::runtime_error(
                "NLang VM: native function does not support out "
                "parameters: " + callee.name);
        CallNative(callee, callParamBase, locals, pResult);
        return;
    }
    std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
    uint16_t paramBytes = callee.paramCount * kFrameSlotBytes;
    if (paramBytes > callee.localsSize)
        throw std::runtime_error(
            "NLang VM: callee locals smaller than the parameter "
            "block in CallDelegate");
    if (paramBytes > 0)
        std::memcpy(calleeLocals.data(), locals + callParamBase, paramBytes);
    ExecuteFunction(callee, pResult, calleeLocals.data());
    for (uint32_t i = 0; i < 32; ++i) {
        if (!(outMask & (1u << i)))
            continue;
        uint16_t off = static_cast<uint16_t>(i * kFrameSlotBytes);
        if (off + kFrameSlotBytes > callee.localsSize)
            throw std::runtime_error(
                "NLang VM: out parameter slot out of bounds");
        std::memcpy(locals + callParamBase + off,
                    calleeLocals.data() + off, kFrameSlotBytes);
    }
}

//Bound-method ABI: the receiver rides at callee slot 0, args shift
//right by one. Natives and intrinsics have no callee frame to hold
//the shifted receiver — the resolver rejects both forms at compile
//time; these guards are the defense-in-depth backstop. The out
//write-back reverses the shift: user param i lives at frame slot i+1
//but stages back to callParamBase+i.
void VmExecutor::CallDelegateBound(const CompiledFunction& callee,
    int32_t thisIdx, uint16_t callParamBase, uint8_t* locals,
    uint8_t* pResult, uint32_t outMask) {
    if (callee.isNative)
        throw std::runtime_error(
            "NLang VM: native method reached by delegate dispatch: "
            + callee.name);
    if (callee.intrinsicId != INTR_None)
        throw std::runtime_error(
            "NLang VM: intrinsic method reached by delegate dispatch: "
            + callee.name);
    std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
    //paramCount includes `this` for methods, matching the
    //OP_CallMethodDirect frame layout.
    uint16_t paramBytes = callee.paramCount * kFrameSlotBytes;
    if (paramBytes > callee.localsSize)
        throw std::runtime_error(
            "NLang VM: callee locals smaller than the parameter "
            "block in CallDelegate");
    std::memcpy(calleeLocals.data(), &thisIdx, sizeof(thisIdx));
    uint16_t argBytes = paramBytes - kFrameSlotBytes;
    if (argBytes > 0)
        std::memcpy(calleeLocals.data() + kFrameSlotBytes,
                    locals + callParamBase, argBytes);
    ExecuteFunction(callee, pResult, calleeLocals.data());
    for (uint32_t i = 0; i < 32; ++i) {
        if (!(outMask & (1u << i)))
            continue;
        uint16_t srcOff = static_cast<uint16_t>((i + 1) * kFrameSlotBytes);
        uint16_t dstOff = static_cast<uint16_t>(i * kFrameSlotBytes);
        if (srcOff + kFrameSlotBytes > callee.localsSize)
            throw std::runtime_error(
                "NLang VM: out parameter slot out of bounds");
        std::memcpy(locals + callParamBase + dstOff,
                    calleeLocals.data() + srcOff, kFrameSlotBytes);
    }
}

//Phase 13 Step 2: shared OP_CallDelegate / OP_CallDelegateOut engine.
//Handle layout: [0]=target (funcIdx for form 0, nameIdx for form 1),
//[1]=this (0 ⟺ free function — the bind-time null guard establishes
//this invariant), [2]=form. Frame layout differs by form:
//  - free function: args copy verbatim from callParamBase (OP_CallFunc
//    ABI; natives read straight from the caller's cells);
//  - bound method: the captured receiver occupies callee slot 0 and the
//    caller's args (staged WITHOUT this) shift right by one.
//outMask bit i marks USER parameter i (func-signature order); the
//write-back reads frame slot i+shift and stores to callParamBase+i,
//reversing the bound-method shift.
void VmExecutor::ExecuteDelegateCall(const std::vector<int32_t>& handle,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
    uint32_t outMask) {
    int32_t thisIdx = handle[1];
    int funcIndex = ResolveDelegateTarget(handle);
    const CompiledFunction& callee
        = m_currModule->functions[static_cast<size_t>(funcIndex)];
    if (thisIdx == 0)
        CallDelegateFree(callee, callParamBase, locals, pResult, outMask);
    else
        CallDelegateBound(callee, thisIdx, callParamBase, locals, pResult,
                          outMask);
}

} // namespace nlang
