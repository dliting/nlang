/*---
    EmitGenericBoxing.cpp — 擦除容器调用边界的装箱规划（PlanBoundaryBox /
    PlanGenericMethodBoxing）。从 EmitExprMemberCall.cpp 拆出（0.13.1a Task 5
    循环完善的尺寸守卫拆分，零行为变化）：声明面签名的占位符形参/返回推导
    逐槽装箱计划，与调用发射本身解耦。
---*/
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/runtime/NodeConsts.h>
#include <map>
#include <vector>

namespace nlang {

//The template's non-static methods of this name (the declared
//templates declare no method overloads today; the boxing derivation
//below stays correct for a future overload set by intersecting).
static void CollectTemplateMethods(SnClassDecl& tmpl,
                                   const std::string& methodName,
                                   std::vector<SnFunction*>& out)
{
    for (auto& member : tmpl.Members())
        if (member.Kind() == NK_Function
            && member.Name() == methodName
            && !member.ContainFlags(NF_Static))
            out.push_back(static_cast<SnFunction*>(&member));
}

//The placeholder every same-name method puts at one formal position —
//null on any absence or disagreement (the resolver's overload-
//intersection rule: a mixed position is not element-typed). ParamList
//is iterable but not indexable, so the position walks each list.
static SnField* UnanimousFormalPlaceholder(
    const std::vector<SnFunction*>& methods, size_t iParam,
    const GenericInstanceRegistry& generics)
{
    SnField* pUnanimous = nullptr;
    for (auto* pFn : methods)
    {
        SnFieldExpr* pType = nullptr;
        size_t i = 0;
        for (auto& param : pFn->Params())
            if (i++ == iParam) { pType = param.Type(); break; }
        SnField* pHere = pType ? pType->Field() : nullptr;
        if (!pHere || !generics.IsPlaceholder(pHere)
            || (pUnanimous && pUnanimous != pHere))
            return nullptr;
        pUnanimous = pHere;
    }
    return pUnanimous;
}

//The placeholder every same-name method returns — null on any
//disagreement (the same unanimity the formal side requires).
static SnField* UnanimousReturnPlaceholder(
    const std::vector<SnFunction*>& methods,
    const GenericInstanceRegistry& generics)
{
    SnFieldExpr* pFirst = methods.front()->ReturnType();
    SnField* pLeaf = pFirst ? pFirst->Field() : nullptr;
    if (!pLeaf || !generics.IsPlaceholder(pLeaf))
        return nullptr;
    for (size_t i = 1; i < methods.size(); ++i)
    {
        SnFieldExpr* pOther = methods[i]->ReturnType();
        if (!pOther || pOther->Field() != pLeaf)
            return nullptr;
    }
    return pLeaf;
}

//0.13.1a Task 5: the erased-container boundary plan for an element-
//typed signature slot against its receiver instantiation. A
//placeholder-typed slot resolves through the receiver's type argument
//(the member-call plan's placeholder formals, or a generic body's
//receiver at the sugar sites): a concrete argument plans the static
//tag, the calling body's own placeholder plans the descriptor-
//dispatched dynamic variant (the uniform-T-representation invariant).
//A concrete slot (the sugar sites' receiver type args, e.g. List<int>'s
//element) plans the static tag by its own kind; everything else —
//string/class/array kinds — passes through raw.
VmBackend::ArgBoxPlan VmBackend::PlanBoundaryBox(SnField* pElemType,
                                                 SnClassDecl* pReceiverInst)
{
    if (!pElemType)
        return {};
    auto* pGenerics = m_pRegistry
        ? &m_pRegistry->DeclaredGenerics() : nullptr;
    //Placeholder resolution (the member-call plan always hands a
    //placeholder or null here; the sugar sites hand a receiver type
    //arg, concrete or placeholder).
    if (pGenerics && pGenerics->IsPlaceholder(pElemType))
    {
        if (!pReceiverInst || !pReceiverInst->IsGenericInstantiation())
            return {};
        const auto& typeArgs = pReceiverInst->GenericTypeArgs();
        const size_t argIdx = pGenerics->PlaceholderIndex(pElemType);
        if (argIdx >= typeArgs.size() || !typeArgs[argIdx])
            return {};
        pElemType = typeArgs[argIdx];
    }
    BoxingTagResult tag = BoxingTagFor(pElemType);
    if (tag.isPrimitive)
        return {true, false, tag.tag, 0};
    //Resolved to the calling body's own placeholder: the static tag is
    //unknowable here — the executing frame's descriptor decides.
    if (pGenerics && pGenerics->IsPlaceholder(pElemType))
    {
        const size_t frameOrdinal = pGenerics->PlaceholderIndex(pElemType);
        if (frameOrdinal < 256)
            return {true, true, 0, static_cast<uint8_t>(frameOrdinal)};
    }
    return {};
}

//Boxed-element generic names (intrinsic ABI property): List, Dict and
//Channel store every element in erased side tables, so a primitive
//type argument must ride OP_Box across the call boundary. Future and
//Promise are absent — their values ride the context result slot raw.
static const char* const kBoxedElementGenericNames[] = {
    kBuiltinListTypeName, kBuiltinDictTypeName, kBuiltinChannelTypeName,
};

//Phase-2 D6: the boundary plan reads the DECLARED signatures. The
//template method's placeholder-typed formals decide which argument
//slots are element storage; the instance's type arguments decide each
//slot's normalization (add/indexOf/contains at slot 1, set's value at
//slot 2, get/recv on the return side — the same plan the three retired
//(baseName, method) tables encoded, now expressed by
//stdlib/containers.n). 0.13.1a Task 5: a slot whose type argument is
//the calling generic body's own placeholder plans the descriptor-
//dispatched dynamic variant (PlanBoundaryBox). Class/struct type
//arguments never box: BoxingTagFor reports not-primitive and the raw
//heap handle passes through, as do array-typed slots (the interned
//token is not a primitive).
void VmBackend::PlanGenericMethodBoxing(SnClassDecl& classDecl,
                                        const std::string& methodName,
                                        std::map<uint16_t, ArgBoxPlan>& argPlans,
                                        ArgBoxPlan& returnPlan) {
    const auto& baseName = classDecl.BaseName();
    bool bBoxedElementGeneric = false;
    for (const char* szName : kBoxedElementGenericNames)
        if (baseName == szName) { bBoxedElementGeneric = true; break; }
    if (!bBoxedElementGeneric || !m_pRegistry)
        return;
    auto& declaredGenerics = m_pRegistry->DeclaredGenerics();
    SnClassDecl* pTemplate = declaredGenerics.InstanceOf(&classDecl);
    if (!pTemplate)
        return;
    std::vector<SnFunction*> methods;
    CollectTemplateMethods(*pTemplate, methodName, methods);
    if (methods.empty())
        return;
    //Slot 0 stages the receiver; formal position i boxes slot i+1.
    size_t maxParams = 0;
    for (auto* pFn : methods)
        if (pFn->Params().size() > maxParams)
            maxParams = pFn->Params().size();
    for (size_t iParam = 0; iParam < maxParams; ++iParam)
    {
        ArgBoxPlan plan = PlanBoundaryBox(
            UnanimousFormalPlaceholder(methods, iParam, declaredGenerics),
            &classDecl);
        if (plan.needsBox)
            argPlans[static_cast<uint16_t>(iParam + 1)] = plan;
    }
    returnPlan = PlanBoundaryBox(
        UnanimousReturnPlaceholder(methods, declaredGenerics), &classDecl);
}

} //namespace nlang
