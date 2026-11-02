/*---
    RegisterInterfaces.cpp — 0.9.10 D5：接口成员的单元级登记。
    登记形态与 enumKeys 同构：限定键 + 静态函数下标 + 常量。
    0.9.12 (v2.7)：接口表新增实例方法签名表（名称/形参数/形参类型/
    返回类型/默认值）——消费端据此解析导入接口接收者上的调用，并
    跨模块检查 implements 契约。从 RegisterClass.cpp 分文件
    （一表一 TU，尺寸守卫余量）。
    v0.12.6 Task 7 (D11)：非折叠接口常量登记为运行时常量槽位
    （ownerKind=1），接口合成 $staticInit 的下标随记录携带。
---*/
#include "VmBackend.h"
#include "builder/BuiltinNames.h"
#include "builder/FieldInitializerChecks.hpp"
#include <nlang/compiler/SyntaxTree.h>
#include <nlang/compiler/SnMisc.h>

namespace nlang {

//RegisterInterfaces worker: one interface's instance-method signature
//records in declaration order. Interface instance members are abstract
//(no function record), so the signature is built straight from the AST:
//formal types (out flags included), return type, and defaults through
//the same channels a function record uses (fold arms, then the v2.3
//ExprDesc encoder, then the RTK_Unfoldable stamp). The consumer
//rebuilds a bodyless stub from these and resolves calls on imported
//interface-typed receivers.
void VmBackend::AppendInterfaceSignatures(SnInterfaceDecl& sn,
                                          CompiledInterface& ci) {
    for (auto& member : sn.Members()) {
        if (member.Kind() != NK_Function
            || static_cast<SnFunction&>(member)
                   .ContainFlags(NF_Static))
            continue;
        auto& func = static_cast<SnFunction&>(member);
        CompiledInterfaceMethod sig;
        sig.name = func.Name();
        for (auto& param : func.Params()) {
            ParamTypeDesc ptd;
            if (param.ContainFlags(NF_Out))
                ptd.flags |= PTDF_Out;
            ptd.type = BuildTypeDesc(param.EvalDataType(),
                LeafSlotResolvers());
            sig.paramTypeDescs.push_back(std::move(ptd));
            auto dv = ExtractDefaultValue(param.Value(),
                RuntimeTypeKind(param.EvalDataType()));
            if (param.Value() && !dv.hasDefault()
                && !EncodeDefaultExpr(func, param.Value(), dv))
                dv.tag = RTK_Unfoldable;
            sig.defaultValues.push_back(dv);
        }
        //Formal count (no `this` slot — the record carries no function
        //entry; the loader enforces the parallel sizes).
        sig.paramCount = static_cast<uint16_t>(sig.paramTypeDescs.size());
        if (func.HasReturn() && func.ReturnType())
            sig.returnType = BuildTypeDesc(
                func.ReturnType()->Field(), LeafSlotResolvers());
        else
            //Void methods stamp the RTK_Void kind byte: the return
            //descriptor is written unconditionally, so the consumer
            //needs the kind (not a zero length) to tell "returns
            //nothing" from an unexpressible return type.
            sig.returnType.kind = RTK_Void;
        ci.instanceMethods.push_back(std::move(sig));
    }
}

//RegisterInterfaces worker: the constants loop. Folded constants carry
//the literal in the wire channel (the consumer folds it at its own use
//sites); a runtime constant instead takes a module slot (ownerKind=1)
//that the interface's $staticInit stores lazily. The fold split is
//StaticConstFoldsAtCompileTime — the same predicate the synthesis
//filter and emission consult.
void VmBackend::RegisterInterfaceConstants(SnInterfaceDecl& sn,
                                           CompiledInterface& ci) {
    for (auto& member : sn.Members()) {
        if (member.Kind() != NK_ClassField
            || !IsInterfaceConstantField(member))
            continue;
        auto& field = static_cast<SnClassField&>(member);
        CompiledInterfaceConstant constant;
        constant.name = field.Name();
        constant.type = BuildTypeDesc(field.EvalDataType(),
            LeafSlotResolvers());
        if (StaticConstFoldsAtCompileTime(field)) {
            constant.value = ExtractDefaultValue(
                field.Initializer(),
                RuntimeTypeKind(field.EvalDataType()));
        } else {
            const uint16_t slotIdx = static_cast<uint16_t>(
                m_compiledModule.staticSlots.size());
            CompiledStaticSlot slot;
            slot.name = field.Name();
            slot.ownerKey = ci.name;
            slot.ownerKind = 1;   //interface
            slot.isRuntimeConst = 1;
            slot.hasInitializer = 1;
            slot.type = constant.type;
            constant.slotIdx = slotIdx;
            m_staticSlotByField[&field] = slotIdx;
            m_compiledModule.staticSlots.push_back(std::move(slot));
        }
        ci.constants.push_back(std::move(constant));
    }
}

void VmBackend::RegisterInterfaces(SnNamespace& root) {
    m_compiledModule.interfaces.clear();
    m_ifaceIndexByDecl.clear();
    ForEachDeclNode(root, [&](SnField& node) {
        //Per-unit: foreign units' interfaces belong to their own images;
        //imported stubs contribute nothing (the consumer rebuilds them
        //from the peer unit's table).
        if (node.Kind() != NK_InterfaceDecl || node.IsImported()
            || !IsOwnUnit(node))
            return;
        auto& sn = static_cast<SnInterfaceDecl&>(node);
        CompiledInterface ci;
        ci.name = KeyOf(sn);
        m_ifaceIndexByDecl[&sn] = static_cast<uint16_t>(
            m_compiledModule.interfaces.size());
        for (auto& member : sn.Members()) {
            if (member.Kind() != NK_Function)
                continue;
            auto& func = static_cast<SnFunction&>(member);
            //Only statics register function records (abstract instance
            //members are body-less and skipped by RegisterFunctions);
            //the map lookup is the registration filter of record.
            auto it = m_funcIndexMap.find(&func);
            if (it == m_funcIndexMap.end())
                continue;
            //v0.12.6 D2: the synthesized "$staticInit" member is not
            //callable surface — a consumer stub must not rebuild it as a
            //ghost method. Its index rides staticInitFuncIdx instead
            //(the class-record shape, RegisterClass.cpp).
            if (IsStaticInitName(func.Name())) {
                ci.staticInitFuncIdx =
                    static_cast<uint16_t>(it->second);
                continue;
            }
            ci.staticMethodIndices.push_back(
                static_cast<uint16_t>(it->second));
        }
        //0.9.10 D3 + v0.12.6 Task 7 (D11): constants export their
        //declared type plus the value (the worker splits the fold).
        RegisterInterfaceConstants(sn, ci);
        //0.9.12 (v2.7): instance-method signatures (see the worker).
        AppendInterfaceSignatures(sn, ci);
        m_compiledModule.interfaces.push_back(std::move(ci));
    });
    RegisterForeignInterfacePlaceholders(root);
}

//v0.12.6 Task 8: foreign interfaces (imported stubs, inline-library
//units) still need a unit-local table index when THIS unit's code
//emits kind-1 gates against them (imported runtime constants,
//imported interface static method calls). A contentless placeholder
//row fills the slot; nlink's interface merge prefers the provider's
//rich row at the same merged index (NcuLinker.cpp).
void VmBackend::RegisterForeignInterfacePlaceholders(SnNamespace& root) {
    ForEachDeclNode(root, [&](SnField& node) {
        if (node.Kind() != NK_InterfaceDecl || IsOwnUnit(node))
            return;
        auto& sn = static_cast<SnInterfaceDecl&>(node);
        CompiledInterface ci;
        ci.name = KeyOf(sn);
        m_ifaceIndexByDecl[&sn] = static_cast<uint16_t>(
            m_compiledModule.interfaces.size());
        m_compiledModule.interfaces.push_back(std::move(ci));
    });
}

//v0.12.6 Task 7 (D11): kind-1 gate operands carry the interface-table
//index; unregistered means a synthesis/registration order bug, not a
//tolerated miss (the class-side ClassSlotFor doctrine).
uint16_t VmBackend::InterfaceSlotFor(SnInterfaceDecl& decl) {
    auto it = m_ifaceIndexByDecl.find(&decl);
    if (it == m_ifaceIndexByDecl.end())
        throw std::runtime_error(
            "NLang backend: interface without a table index: "
            + decl.Name());
    return it->second;
}

} //namespace nlang
