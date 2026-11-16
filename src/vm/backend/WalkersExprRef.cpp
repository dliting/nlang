/*---
    WalkersExprRef.cpp — v0.13.9 phase 3 Task 5: the &-argument side of
    the frame-size walkers (source-size split, WalkersExpr.cpp
    precedent). MakeRefPeakDepth mirrors the MakeRef emission claim
    shapes (EmitExprRef.cpp); CalleeHasRefFormal gates the per-argument
    charge that the three invoke-family arg loops apply through
    ArgPeakDepth.
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

} //namespace nlang
