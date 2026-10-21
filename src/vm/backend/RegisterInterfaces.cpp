/*---
    RegisterInterfaces.cpp — 0.9.10 D5：接口成员的单元级登记。
    登记形态与 enumKeys 同构：限定键 + 静态函数下标 + 常量。
    0.9.12 (v2.7)：接口表新增实例方法签名表（名称/形参数/形参类型/
    返回类型/默认值）——消费端据此解析导入接口接收者上的调用，并
    跨模块检查 implements 契约。从 RegisterClass.cpp 分文件
    （一表一 TU，尺寸守卫余量）。
---*/
#include "VmBackend.h"
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

void VmBackend::RegisterInterfaces(SnNamespace& root) {
    m_compiledModule.interfaces.clear();
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
            ci.staticMethodIndices.push_back(
                static_cast<uint16_t>(it->second));
        }
        //0.9.10 D3: constants export their declared type plus the folded
        //initializer. The wire channel reuses the formal-default
        //descriptor (RTK tag + payload); the consumer rebuilds a literal
        //stub and folds it at its own use sites.
        for (auto& member : sn.Members()) {
            if (member.Kind() != NK_ClassField
                || !IsInterfaceConstantField(member))
                continue;
            auto& field = static_cast<SnClassField&>(member);
            CompiledInterfaceConstant constant;
            constant.name = field.Name();
            constant.type = BuildTypeDesc(field.EvalDataType(),
                LeafSlotResolvers());
            constant.value = ExtractDefaultValue(
                field.Initializer(), RuntimeTypeKind(field.EvalDataType()));
            ci.constants.push_back(std::move(constant));
        }
        //0.9.12 (v2.7): instance-method signatures (see the worker).
        AppendInterfaceSignatures(sn, ci);
        m_compiledModule.interfaces.push_back(std::move(ci));
    });
}

} //namespace nlang
