/*---
    EmitCtorCall.cpp — 构造器调用发射族：构造器声明查找与注册过滤、
    形参绑定构建（含默认值绑定）、构造器调用尾部（分配器 + 直调）。
    从 EmitCall.cpp 抽取（0.9.11 T2 可维护性拆分，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>

namespace nlang {

//The shared registration predicate (see VmBackend.h). Imported stubs
//count: the compiler registers their import record without emitting a
//body, so the old body/native filter would reject a .ncu ctor member and
//desync the walker claim from the codegen claim.
bool VmBackend::IsRegisteredCtor(const SnFunction& ctor) {
    return ctor.IsImported() || ctor.Body() || ctor.ContainFlags(NF_Native);
}

//The AST constructor declaration of a class — the same Name() rule and
//IsRegisteredCtor filter CtorSlotFor's placeholder scan applies, so the
//returned function is the one whose slot the invocation targets.
//0.9.11 v2.6: imported stub classes answer their rebuilt ctor member
//(BuildCtorBindings reads its formals/defaults for the call).
SnFunction* VmBackend::FindCtorFunction(SnClassDecl* pClassDecl) {
    if (!pClassDecl || pClassDecl->IsBuiltinClass())
        return nullptr;
    for (auto& member : pClassDecl->Members()) {
        if (member.Kind() != NK_Function
            || member.Name() != pClassDecl->Name())
            continue;
        auto& ctor = static_cast<SnFunction&>(member);
        //0.9.10 D2.4: a same-name static is not the constructor.
        if (ctor.ContainFlags(NF_Static))
            continue;
        if (!IsRegisteredCtor(ctor))
            continue;
        return &ctor;
    }
    return nullptr;
}

//0.9.11 D2: the AST-selected struct ctor when it is registered
//(IsRegisteredCtor), else null. Mirrors FindCtorFunction's filter so an
//unregistered ctor allocates without running; codegen and the frame
//walkers share the predicate (walker symmetry, P3 discipline).
SnFunction* VmBackend::RegisteredCtor(SnFunction* pCtorFn) {
    if (!pCtorFn || !IsRegisteredCtor(*pCtorFn))
        return nullptr;
    return pCtorFn;
}

//Defaults-aware FormalBinding array for the ctor-invocation lowering
//(see VmBackend.h). pCtorFn null (builtin/generic/imported) leaves the
//bindings empty — the caller's argument count is the whole formal count.
size_t VmBackend::BuildCtorBindings(SnFunction* pCtorFn,
        const std::vector<SnExpression*>& callerArgs,
        std::vector<FormalBinding>& bindings) {
    if (!pCtorFn)
        return callerArgs.size();
    auto& params = pCtorFn->Params();
    //Index the formal list once — ChildFieldList iterates, no random
    //access, and ctor formals are capped at kMaxFuncParams anyway.
    std::vector<SnFormalParam*> formals;
    formals.reserve(params.size());
    for (auto& param : params)
        formals.push_back(&param);
    bindings.reserve(formals.size());
    size_t idx = 0;
    for (auto* pArg : callerArgs) {
        bindings.push_back({FormalBinding::B_Positional, pArg,
                            formals[idx], false});
        ++idx;
    }
    for (; idx < formals.size(); ++idx)
        bindings.push_back({FormalBinding::B_Default, nullptr,
                            formals[idx], false});
    return formals.size();
}

//True when any binding stages a default expression (see VmBackend.h).
bool VmBackend::BindingsEmitDefaults(
        const std::vector<FormalBinding>& bindings) {
    for (const auto& b : bindings)
        if (b.kind == FormalBinding::B_Default) return true;
    return false;
}

//One ctor-call binding set into claim[1..N] (see VmBackend.h).
void VmBackend::EmitCtorCallBindings(
        const std::vector<FormalBinding>& bindings,
        const std::vector<SnExpression*>& callerArgs,
        uint16_t claimBase, BytecodeEmitter& emitter, uint16_t thisSlot) {
    if (bindings.empty()) {
        for (size_t i = 0; i < callerArgs.size(); ++i) {
            uint16_t paramOffset = claimBase
                + static_cast<uint16_t>(i + 1) * kFrameSlotBytes;
            EmitExpression(*callerArgs[i], emitter, paramOffset);
        }
        return;
    }
    for (size_t i = 0; i < bindings.size(); ++i) {
        EmitBinding(bindings.data(), i, static_cast<uint16_t>(i + 1), 1,
                    emitter, thisSlot, claimBase);
    }
}

//Ctor-call tail after the bindings (see VmBackend.h). The allocator
//follows CtorAlloc: OP_New for a class, OP_AllocStruct for a struct
//(0.9.11 D2 — the struct operands mirror the OP_AllocStruct emission in
//Access(SnLocalDeclStmt); the executor sizes the object from the struct
//table, the operands are disassembly/linker aids).
void VmBackend::EmitCtorCallTail(uint16_t allocSlot, uint16_t claimBase,
        uint16_t slotCount, const CtorAlloc& alloc, uint16_t ctorIdx,
        BytecodeEmitter& emitter) {
    if (alloc.isStruct) {
        emitter.Emit(OpCode::OP_AllocStruct);
        emitter.EmitUint16(allocSlot);
        emitter.EmitUint16(alloc.typeIdx);
        emitter.EmitUint16(alloc.fieldCount);
    } else {
        emitter.Emit(OpCode::OP_New);
        emitter.EmitUint16(allocSlot);
        emitter.EmitUint16(alloc.typeIdx);
    }
    //this (new object heap index) → claim[0].
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(allocSlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(claimBase);
    //Bulk-copy evalArea claim → callParamBase just before the call.
    CopyClaimToCallParams(claimBase, slotCount, emitter);
    emitter.Emit(OpCode::OP_CallMethodDirect);
    emitter.EmitUint16(ctorIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_ParaEnd);
}

} //namespace nlang
