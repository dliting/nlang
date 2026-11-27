/*---
    WalkersExprRef.cpp — v0.13.9 phase 3 Task 5: the &-argument side of
    the frame-size walkers (source-size split, WalkersExpr.cpp
    precedent). MakeRefPeakDepth mirrors the MakeRef emission claim
    shapes (EmitExprRef.cpp); CalleeHasRefFormal gates the per-argument
    charge that the three invoke-family arg loops apply through
    ArgPeakDepth. CtorDefaultExprDepth (the shared tail of the three
    ctor-call depth arms) joined the split for the same source-size
    reason (0.13.13a review round 1, WalkersExpr.cpp at the 500-line
    cap).
---*/
#include "VmBackend.h"
#include <algorithm>
#include <unordered_set>

namespace nlang {

//Does this callee bind any & formal? Gates the MakeRefPeakDepth charge
//in the invoke-family arg loops (no & formal charges nothing — the
//plain ExprPeakDepth path).
bool VmBackend::CalleeHasRefFormal(SnFunction* pCallee) {
    if (!pCallee)
        return false;
    for (auto& formal : pCallee->Params())
        if (formal.ContainFlags(NF_Ref))
            return true;
    return false;
}

//Task 8: the func<...> key behind a delegate slot — the local/field's
//declared type when it is a func instantiation, null otherwise.
SnClassDecl* VmBackend::DelegateFuncTypeOfField(const SnField* pSlot) {
    if (!pSlot)
        return nullptr;
    auto* pType = pSlot->EvalDataType();
    if (!pType || pType->Kind() != NK_ClassDecl)
        return nullptr;
    auto* pFuncType = static_cast<SnClassDecl*>(pType);
    return pFuncType->IsFuncType() ? pFuncType : nullptr;
}

//v0.13.13a D-3: the func<...> key behind either delegate-call shape.
//The postfix shape's resolver gate admitted the callee expression only
//when its type is a func instantiation, so reading the EvalDataType
//back here re-checks rather than re-decides; the named shape keeps
//going through its value field.
SnClassDecl* VmBackend::DelegateFuncTypeOfInvoke(const SnInvokeExpr& invoke) {
    if (auto* pCalleeExpr = invoke.CalleeExpr()) {
        auto* pType = pCalleeExpr->EvalDataType();
        if (!pType || pType->Kind() != NK_ClassDecl)
            return nullptr;
        auto* pFuncType = static_cast<SnClassDecl*>(pType);
        return pFuncType->IsFuncType() ? pFuncType : nullptr;
    }
    return DelegateFuncTypeOfField(invoke.Field());
}

//Any & mark on the key's ARGUMENT positions (refFlags[1..]) — gates the
//walker's MakeRef charge so it cannot drift from the argument-side
//pair staging in EmitUnresolvedInvokeArgs.
bool VmBackend::DelegateKeyHasRefArg(const SnInvokeExpr& invoke) {
    if (!IsDelegateInvoke(invoke))
        return false;
    auto* pFuncType = DelegateFuncTypeOfInvoke(invoke);
    if (!pFuncType)
        return false;
    const auto& refFlags = pFuncType->GenericRefFlags();
    for (size_t i = 1; i < refFlags.size(); ++i)
        if (refFlags[i] != 0)
            return true;
    return false;
}

//The key's RETURN & mark (refFlags[0]) — gates the &-context staging of
//a delegate invoke: a &-return key parks the raw pair, the same MakeRef
//contract as a &-returning direct callee.
bool VmBackend::DelegateKeyHasRefReturn(const SnInvokeExpr& invoke) {
    if (!IsDelegateInvoke(invoke))
        return false;
    auto* pFuncType = DelegateFuncTypeOfInvoke(invoke);
    if (!pFuncType)
        return false;
    const auto& refFlags = pFuncType->GenericRefFlags();
    return !refFlags.empty() && refFlags[0] != 0;
}

//Value-side return normalization tag for a delegate key: refFlags[0]
//set with a scalar/string/enum return slot names the OP_LoadRef tag
//(the same kind gate RefReturnDerefKind applies to direct callees);
//struct/identity & returns and plain keys return kNoRefDeref — the raw
//pair passes through and the offset-0-pair convention reads it.
uint8_t VmBackend::DelegateReturnDerefKind(const SnClassDecl* pFuncType) {
    if (!pFuncType)
        return kNoRefDeref;
    const auto& refFlags = pFuncType->GenericRefFlags();
    if (refFlags.empty() || refFlags[0] == 0)
        return kNoRefDeref;
    const auto& typeArgs = pFuncType->GenericTypeArgs();
    if (typeArgs.empty() || !typeArgs[0])
        return kNoRefDeref;
    const NodeKind kind = typeArgs[0]->Kind();
    if (kind != NK_String && kind != NK_EnumDecl
        && ScalarPrimIndexOf(kind) < 0)
        return kNoRefDeref;
    return BoxTypeTagOfKind(kind);
}

//The &-argument MakeRef emission (EmitExprRef.cpp) as a static depth
//mirror — same dispatch, same claim shapes:
//  - identifier: raw pair copy / RefOfLocal / RefOfPromoted — no claim.
//  - member chain: claim(1) parks the walk record; the root's own
//    emission depth climbs above it; hops load in place (no claim).
//  - subscript: the staging claim [array, indices...] is exactly the
//    read path's claim — reuse its mirror (slices never reach MakeRef:
//    the resolver rejects them, so its slice branch can only
//    over-reserve).
//  - everything else (rvalue arm): claim(1) parks the temporary
//    record; the value's normal emission climbs above it.
//Named/out wrappers are transparent — the binding's pCallerExpr is the
//user expression, so unwrapping here keeps the mirror exact.
uint16_t VmBackend::MakeRefPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    const NodeKind kind = expr.Kind();
    if (kind == NK_IdentifierExpr)
        return 0;
    if (kind == NK_NamedArgExpr)
        return MakeRefPeakDepth(
            *static_cast<SnNamedArgExpr&>(expr).Inner(), visited);
    if (kind == NK_OutArgExpr)
        return MakeRefPeakDepth(
            *static_cast<SnOutArgExpr&>(expr).Inner(), visited);
    if (kind == NK_MemberExpr)
        return static_cast<uint16_t>(1 + ExprPeakDepth(
            *static_cast<SnMemberExpr&>(expr).Outer(), visited));
    if (kind == NK_SubscriptExpr)
        return SubscriptExprPeakDepth(expr, visited);
    return static_cast<uint16_t>(1 + ExprPeakDepth(expr, visited));
}

//Per-argument depth for the three invoke-family arg loops: the plain
//expression depth, widened to the MakeRef mirror when the callee binds
//any & formal (MakeRefPeakDepth >= ExprPeakDepth on every shape, so
//non-ref args only gain headroom — over-reserving is the safe
//direction).
uint16_t VmBackend::ArgPeakDepth(SnExpression& arg,
    const std::unordered_set<SnFunction*>& visited, bool bHasRefFormal) {
    uint16_t d = ExprPeakDepth(arg, visited);
    if (bHasRefFormal)
        d = std::max(d, MakeRefPeakDepth(arg, visited));
    return d;
}

//Shared tail of the three ctor-call depth arms (NewExprPeakDepth /
//CtorInvokePeakDepth / SuperCallStmtPeakDepth): return the deeper of
//the caller-arg depth and the parent-ctor default-expression depth.
//Mirrors InvokeExprPeakDepth's visited-set pattern — a default that
//invokes the same ctor again would descend forever without the guard
//(the codegen side rejects it via DefaultEmitGuard; here the walker
//just stops descending). pCtorFn null (builtin/generic/imported) means
//no defaults exist — argDepth passes through.
uint16_t VmBackend::CtorDefaultExprDepth(SnFunction* pCtorFn,
    const std::unordered_set<SnFunction*>& visited, uint16_t argDepth) {
    if (!pCtorFn || visited.find(pCtorFn) != visited.end())
        return argDepth;
    auto visitedPlus = visited;
    visitedPlus.insert(pCtorFn);
    uint16_t defaultDepth = 0;
    for (auto& formal : pCtorFn->Params()) {
        if (formal.Value()) {
            uint16_t d = ExprPeakDepth(*formal.Value(), visitedPlus);
            if (d > defaultDepth) defaultDepth = d;
        }
    }
    return argDepth > defaultDepth ? argDepth : defaultDepth;
}

} //namespace nlang
