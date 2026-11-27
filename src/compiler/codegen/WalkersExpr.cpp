/*---
    WalkersExpr.cpp — 帧尺寸 walker 表达式域：
    容器下标/委托调用共享谓词 + 表达式求值峰值深度。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化：
    walker 与共享谓词提升为 VmBackend 静态成员，见 VmBackend.h）。
---*/
#include "VmBackend.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <unordered_set>

namespace nlang {

//True when a subscript's base expression resolves to a List<T>/
//Dict<K,V> generic instantiation — the subscript is sugar over the
//get()/set() intrinsics and must NOT take the OP_LoadElement array
//path. Shared by the read lowering, the subscript-assign lowering,
//the arr[i].field = v member-write receiver path, and the three
//walker sites (ExprPeakDepth/StmtPeakDepth/MaxArgsWalker) so the
//dispatch decision cannot drift between codegen and walkers.
bool VmBackend::IsContainerSubscript(SnExpression& baseExpr) {
    //Array bases take the OP_LoadElement/OP_StoreElement path even when
    //the element type is a generic instantiation (`List<int>[] a`):
    //EvalDataType returns the ELEMENT type, so the container check must
    //come after the array-ness check (EvalDataType dispatch-order trap,
    //5th instance — mirrors the resolver peel guard in ExprResolver).
    {
        SnIdentifierExpr* pId = nullptr;
        if (baseExpr.Kind() == NK_IdentifierExpr)
            pId = static_cast<SnIdentifierExpr*>(&baseExpr);
        else if (baseExpr.Kind() == NK_MemberExpr) {
            auto* pInner = static_cast<SnMemberExpr&>(baseExpr).Inner();
            if (pInner && pInner->Kind() == NK_IdentifierExpr)
                pId = static_cast<SnIdentifierExpr*>(pInner);
        }
        if (pId && pId->Field() && pId->Field()->IsArrayType())
            return false;
    }
    auto* pBaseType = baseExpr.IsResolved()
        ? baseExpr.EvalDataType() : nullptr;
    if (!pBaseType || pBaseType->Kind() != NK_ClassDecl) return false;
    auto* pGenClass = static_cast<SnClassDecl*>(pBaseType);
    if (!pGenClass->IsGenericInstantiation()) return false;
    const auto& baseName = pGenClass->BaseName();
    const auto& typeArgs = pGenClass->GenericTypeArgs();
    if (baseName == kBuiltinListTypeName) return !typeArgs.empty();
    if (baseName == kBuiltinDictTypeName) return typeArgs.size() > 1;
    return false;
}

//isMethodContext=true when the InvokeExpr is the Inner() of a MemberExpr
//(i.e. a method call shape `receiver.method(...)`). This is the ONLY
//reliable way to detect method calls — callee->Parent() is null for
//built-in methods (List.add, Dict.set, ToString, etc.), which would
//cause the walker to underreserve evalArea slots and EmitCallArgs
//(called with slotBase=1 from the MemberExpr handler) would overflow
//into user variable space.
//Phase 13: a delegate invoke is a resolved invoke whose Field() carries
//a func-typed value (local/param/field) rather than an SnFunction.
//v0.13.13a D-3 adds the postfix shape: the callee is an expression
//evaluating to the func value (Field() stays null). Shared by codegen
//and both frame-size walkers so their claim shapes stay in lockstep
//(walker symmetry, 6th instance).
bool VmBackend::IsDelegateInvoke(const SnInvokeExpr& invoke)
{
    if (invoke.CalleeExpr())
        return true;
    return invoke.Field() != nullptr
        && invoke.Field()->Kind() != NK_Function;
}

//ExprPeakDepth front: transparent wrappers evaluate their inner into
//the parent's claim slot — no claim of their own. True (with outDepth)
//when expr is one of the wrapper shapes.
bool VmBackend::TryTransparentWrapperDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited, uint16_t& outDepth) {
    switch (expr.Kind())
    {
    case NK_CastExpr:
        outDepth = ExprPeakDepth(*static_cast<SnCastExpr&>(expr).Source(), visited);
        return true;
    case NK_AsExpr:
        outDepth = ExprPeakDepth(*static_cast<SnAsExpr&>(expr).Operand(), visited);
        return true;
    //NamedArgExpr / OutArgExpr — the inner evaluates into the claimed
    //binding slot.
    case NK_NamedArgExpr:
        outDepth = ExprPeakDepth(*static_cast<SnNamedArgExpr&>(expr).Inner(), visited);
        return true;
    case NK_OutArgExpr:
        outDepth = ExprPeakDepth(*static_cast<SnOutArgExpr&>(expr).Inner(), visited);
        return true;
    //SliceItemExpr (v0.12.7 Task 11): its present parts evaluate into
    //the parent subscript's claim slots.
    case NK_SliceItemExpr:
        outDepth = SliceItemExprPeakDepth(expr, visited);
        return true;
    default:
        return false;
    }
}

uint16_t VmBackend::ExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited,
    bool isMethodContext) {
    uint16_t wrapped = 0;
    if (TryTransparentWrapperDepth(expr, visited, wrapped))
        return wrapped;
    NodeKind kind = expr.Kind();
    if (kind == NK_InvokeExpr)
        return InvokeExprPeakDepth(expr, visited, isMethodContext);
    //Non-invoke expressions: recurse into children.
    //UnaryExpr: NLang uses NK_BinaryExpr for both binary and unary.
    //Unary ops (OP_Neg, OP_BitNot, OP_LogicalNot) have Right()==nullptr.
    if (kind == NK_BinaryExpr)
        return BinaryExprPeakDepth(expr, visited);
    //SubscriptExpr
    if (kind == NK_SubscriptExpr)
        return SubscriptExprPeakDepth(expr, visited);
    //MemberExpr (field access: outer.inner)
    if (kind == NK_MemberExpr)
        return MemberExprPeakDepth(expr, visited);
    if (kind == NK_NewExpr)
        return NewExprPeakDepth(expr, visited);
    if (kind == NK_NewArrayExpr)
        return NewArrayExprPeakDepth(expr, visited);
    if (kind == NK_InitListExpr)
        return InitListExprPeakDepth(expr, visited);
    //0.13.13a D-1 review round 1: a call-site default lambda's snapshot
    //fill may claim one scratch slot inside the formal-claim slice (the
    //struct-flavored store arms a rooted fresh-record copy) — 0 for
    //every other lambda shape, preserving the former leaf answer. The
    //predicate CallSiteFillScratch lives beside the fill it mirrors
    //(EmitCaptureFrame.cpp).
    if (kind == NK_LambdaExpr)
        return CallSiteFillScratch(static_cast<SnLambdaExpr&>(expr));
    //0.13.10 Task 6: a captured identifier read parks its chased
    //display record in an EvalAreaClaim(1) (EmitCapturedRead) — the
    //assignment walker's captured arm mirrors the same +1.
    if (kind == NK_IdentifierExpr
        && IsCapturedField(
            static_cast<SnIdentifierExpr&>(expr).Field()))
        return 1;
    //Leaf expressions (LiteralExpr, IdentifierExpr, ThisExpr, etc.)
    return 0;
}

//NK_InvokeExpr arm of ExprPeakDepth.
uint16_t VmBackend::InvokeExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited, bool isMethodContext) {
    auto& invoke = static_cast<SnInvokeExpr&>(expr);
    //0.9.0/0.9.11: a ctor-carrying invoke (class or struct twin)
    //shares the NewExpr claim shape.
    if (invoke.CtorClass() || invoke.CtorStruct() || invoke.ArrayDynCtor())
        return CtorInvokePeakDepth(invoke, visited);
    auto* callee = invoke.Callee();
    size_t formalCount = callee ? callee->Params().size() : 0;
    if (!callee) { for (auto& p : invoke.Params()) ++formalCount; }
    //Method call (slotBase=1) when caller signals method context, OR
    //when the callee resolves to a method (Parent is class/struct/
    //interface); the flag is authoritative when callee is null.
    bool isMethod = isMethodContext || (callee && callee->Parent()
        && (callee->Parent()->Kind() == NK_ClassDecl
            || callee->Parent()->Kind() == NK_StructDecl
            || callee->Parent()->Kind() == NK_InterfaceDecl));
    size_t claimSize = formalCount + (isMethod ? 1 : 0);
    //Phase 13: a delegate invoke materializes its callee handle into
    //one scratch slot (claims sequential: args first, then callee —
    //over-reservation, the safe direction).
    if (!callee && VmBackend::IsDelegateInvoke(invoke))
        claimSize += 1;

    //Peak depth of argument sub-expressions (the &-formal charge is
    //ArgPeakDepth's — CalleeHasRefFormal, or a delegate key's
    //refFlags for the pair staging; WalkersExprRef.cpp, Task 8).
    const bool bHasRefFormal = CalleeHasRefFormal(callee)
        || DelegateKeyHasRefArg(invoke);
    uint16_t argDepth = 0;
    for (auto& param : invoke.Params())
        argDepth = std::max(argDepth,
            ArgPeakDepth(param, visited, bHasRefFormal));
    //v0.13.13a D-3: the postfix callee expression evaluates into the
    //staged claim slot, so its own sub-expression depth joins the max
    //above (the claim is live while the callee emits).
    if (auto* pCalleeExpr = invoke.CalleeExpr())
        argDepth = std::max(argDepth, ExprPeakDepth(*pCalleeExpr, visited));

    //Callee default expressions evaluate in the caller frame.
    uint16_t defaultDepth = 0;
    if (callee && visited.find(callee) == visited.end()) {
        auto visitedPlus = visited;
        visitedPlus.insert(callee);
        for (auto& formal : callee->Params()) {
            if (formal.Value()) {
                uint16_t d = ExprPeakDepth(*formal.Value(), visitedPlus);
                if (d > defaultDepth) defaultDepth = d;
            }
        }
    }

    return static_cast<uint16_t>(claimSize) +
        (argDepth > defaultDepth ? argDepth : defaultDepth);
}

//0.9.0: a ctor-carrying invoke emits through the NewExpr path — claim
//{this, formals...} plus default-expression depth (P3, walker symmetry
//with NewExprPeakDepth).
uint16_t VmBackend::CtorInvokePeakDepth(SnInvokeExpr& invoke,
    const std::unordered_set<SnFunction*>& visited) {
    size_t argCount = 0;
    uint16_t argDepth = 0;
    //0.9.11 T6 (D10): class and struct invokes both carry the
    //resolver-selected ctor in the AST; the registered filter keeps the
    //walker in step with codegen (an unregistered ctor allocates
    //without a claim — over-reserving here is the safe direction).
    SnFunction* pCtorFn = RegisteredCtor(invoke.CtorFunction());
    //&-formal charge (see InvokeExprPeakDepth) — ctor args share the
    //EmitBinding binding path.
    const bool bHasRefFormal = CalleeHasRefFormal(pCtorFn);
    for (auto& param : invoke.Params()) {
        ++argCount;
        argDepth = std::max(argDepth,
            ArgPeakDepth(param, visited, bHasRefFormal));
    }
    const size_t totalFormals = pCtorFn
        ? pCtorFn->Params().size() : argCount;
    return static_cast<uint16_t>(1 + totalFormals)
        + CtorDefaultExprDepth(pCtorFn, visited, argDepth);
}

//NK_BinaryExpr arm of ExprPeakDepth.
uint16_t VmBackend::BinaryExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    auto& bin = static_cast<SnBinaryExpr&>(expr);
    //v0.13.7: a tensor-flagged binary emits through its emission plan
    //as a 2-arg tensor call — mirror the invoke claim shape (argCount
    //slots + operand sub-expression depth). The operands are the plan's
    //params once bound; before binding they are this node's children
    //(the TU-end sweep rejects bound-less flagged nodes, so a bound
    //plan is guaranteed at this point).
    if (bin.ContainFlags(NF_TensorOp)) {
        uint16_t argDepth = 0;
        for (auto &param : bin.TensorCallPlan()->Params()) {
            uint16_t d = ExprPeakDepth(param, visited);
            if (d > argDepth) argDepth = d;
        }
        size_t argCount = 0;
        for (auto &param : bin.TensorCallPlan()->Params()) ++argCount;
        return static_cast<uint16_t>(argCount + argDepth);
    }
    uint16_t l = ExprPeakDepth(*bin.Left(), visited);
    if (bin.Right()) {
        uint16_t r = ExprPeakDepth(*bin.Right(), visited);
        //Short-circuit lowering: logical nodes claim no eval-area
        //slot — both operands emit into resultOffset sequentially.
        //Keep symmetric with EmitExpression's logical special-case.
        auto bop = bin.Op();
        if (bop == SnBinaryExpr::OP_LogicalAnd
            || bop == SnBinaryExpr::OP_LogicalOr)
            return l > r ? l : r;
        //Right operand parks in a per-level EvalAreaClaim(1) (round-4 —
        //the old PickTempSlot chain wrapped tempSlot4 → tempSlot at
        //depth 5); operand sub-expressions claim above it.
        uint16_t m = l > r ? l : r;
        return 1 + m;
    }
    return l;  //unary: operand → resultOffset, no claim
}

//NK_SliceItemExpr arm of ExprPeakDepth (v0.12.7 Task 11): no claim of
//its own — the deepest of its present parts is the item's depth (the
//parts evaluate into the parent subscript's claim slots).
uint16_t VmBackend::SliceItemExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    auto& item = static_cast<SnSliceItemExpr&>(expr);
    uint16_t deepest = 0;
    if (item.Head())
        deepest = std::max(deepest,
            ExprPeakDepth(*item.Head(), visited));
    if (item.Stop())
        deepest = std::max(deepest,
            ExprPeakDepth(*item.Stop(), visited));
    if (item.Step())
        deepest = std::max(deepest,
            ExprPeakDepth(*item.Step(), visited));
    return deepest;
}

//NK_SubscriptExpr arm of ExprPeakDepth.
uint16_t VmBackend::SubscriptExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    auto& sub = static_cast<SnSubscriptExpr&>(expr);
    //0.9.0: a sized-array creation twin emits through the new-array
    //path — the size stages in an evalArea claim(1) and the base names
    //a type (no emission). Walker symmetry with NewArrayExprPeakDepth.
    if (sub.NewArrayElem())
    {
        //Stage-2: ND creation stages one eval-area slot per dim, plus
        //each dim's own nested peak.
        uint16_t deepest = 0;
        for (auto *pIndex : sub.Indices())
            deepest = std::max(deepest,
                ExprPeakDepth(*pIndex, visited));
        return static_cast<uint16_t>(sub.Indices().size()) + deepest;
    }
    uint16_t a = ExprPeakDepth(*sub.Array(), visited);
    uint16_t i = 0;
    for (auto *pIndex : sub.Indices())
        i = std::max(i, ExprPeakDepth(*pIndex, visited));
    //Stage-2: the read claim is receiver + N indices (container lowers
    //to a get() call — same count, its index is one expression).
    //v0.12.7 Task 11: a slice subscript claims receiver + 3 slots per
    //axis (start/stop/step) + the presence-mask words — EmitArrayViewRead's
    //layout, mirrored so evalArea never under-reserves.
    const size_t n = sub.Indices().size();
    const uint16_t claim = HasSliceAxis(sub)
        ? static_cast<uint16_t>(1 + 3 * n + (4 * n + 31) / 32)
        : static_cast<uint16_t>(1 + n);
    uint16_t m = (a > i ? a : i);
    return claim + m;
}

//NK_MemberExpr arm of ExprPeakDepth (field access: outer.inner).
uint16_t VmBackend::MemberExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    auto& member = static_cast<SnMemberExpr&>(expr);
    uint16_t d = ExprPeakDepth(*member.Outer(), visited, false);    //If Inner is an InvokeExpr, this is a method call shape — pass
    //isMethodContext=true so the walker reserves slot 0 for `this`.
    //Phase 13 Step 2 exception: a delegate member invoke (obj.cb(x))
    //stages USER args only (no this at slot 0) — its callee scratch
    //is reserved inside the invoke walker via IsDelegateInvoke.
    uint16_t id;
    if (member.Inner() && member.Inner()->Kind() == NK_InvokeExpr) {
        auto& inv = static_cast<SnInvokeExpr&>(*member.Inner());
        id = ExprPeakDepth(*member.Inner(), visited,
            !VmBackend::IsDelegateInvoke(inv));
    } else {
        id = ExprPeakDepth(*member.Inner(), visited, false);
    }
    //Phase 11 Step 3 symmetry: a built-in string method stages
    //synthetic trailing args (substring's end via OP_StrLen) on top
    //of the actual ones — reserve those slots here from the SAME
    //predicate codegen consumes, or a 1-arg substring call would
    //clobber a slot above the walker-shaped frame. Overreserving for
    //a user method that happens to share the name is safe (finalize
    //only asserts observed <= walker).
    if (member.Inner() && member.Inner()->Kind() == NK_InvokeExpr) {
        auto& inv = static_cast<SnInvokeExpr&>(*member.Inner());
        uint16_t actual = 0;
        for (auto& p : inv.Params()) ++actual;
        id = static_cast<uint16_t>(id + (StringMethodStagedArgCount(
            inv.CalleeName(), actual) - actual));
    }
    return d > id ? d : id;
}

//NK_NewExpr arm of ExprPeakDepth.
//claimSize = 1 (this) + totalFormals, mirroring the codegen path which
//claims an evalArea slice for {this, formals...} then bulk-copies to
//callParamBase before OP_CallMethodDirect (P3: omitted trailing formals
//stage their defaults inside the same claim). Conservative: claims even
//when no ctor exists (alloc-only NewExpr doesn't need the slice, but
//over-reserving by 1 slot is safe and rare).
uint16_t VmBackend::NewExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    auto& newExpr = static_cast<SnNewExpr&>(expr);
    size_t argCount = 0;
    for (auto& arg : newExpr.Args()) {
        if (&arg == newExpr.ClassName()) continue;
        ++argCount;
    }
    //0.9.11 T6 (D10): class and struct new-expressions both carry the
    //resolver-selected ctor in the AST (see CtorInvokePeakDepth for the
    //registered-filter rationale).
    SnFunction* pCtorFn = RegisteredCtor(newExpr.CtorFunction());
    const size_t totalFormals = pCtorFn
        ? pCtorFn->Params().size() : argCount;
    uint16_t claimSize = static_cast<uint16_t>(1 + totalFormals);
    //&-formal charge (see InvokeExprPeakDepth) — ctor args share the
    //EmitBinding binding path.
    const bool bHasRefFormal = CalleeHasRefFormal(pCtorFn);
    uint16_t d = 0;
    for (auto& arg : newExpr.Args()) {
        //Skip the class-name child (Args() view includes it — see the
        //NewExpr codegen handler); mirror codegen exactly.
        if (&arg == newExpr.ClassName()) continue;
        d = std::max(d, ArgPeakDepth(arg, visited, bHasRefFormal));
    }
    return claimSize + CtorDefaultExprDepth(pCtorFn, visited, d);
}

//NK_NewArrayExpr arm of ExprPeakDepth.
//claimSize = 1: the size expression stages in an evalArea claim
//(Phase 10 audit round-8 — was PickTempSlot temp staging, clobbered
//by the EmitBinding struct deep-copy scratch).
uint16_t VmBackend::NewArrayExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    auto& na = static_cast<SnNewArrayExpr&>(expr);
    return 1 + ExprPeakDepth(*na.Size(), visited);
}

//NK_InitListExpr arm of ExprPeakDepth.
//Phase 9c follow-up + Phase 10 audit round-8: mirror the codegen's
//claim pattern.
//  - Dict form: per-entry EvalAreaClaim(3) [this, key, value]
//  - List form: per-entry EvalAreaClaim(2) [this, value] (Phase 10
//    audit C2 — was callParamBase staging, clobbered by nested calls)
//  - Array/Struct/Class forms: EvalAreaClaim(1) stages each entry
//    value (Phase 10 audit round-8 — was PickTempSlot temp staging,
//    clobbered by struct-argument deep-copy scratch in EmitBinding)
uint16_t VmBackend::InitListExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    auto& init = static_cast<SnInitListExpr&>(expr);
    SnField* pTarget = init.EvalDataType();
    uint16_t claimSize = 1;
    if (pTarget && pTarget->Kind() == NK_ClassDecl) {
        auto* pClassDecl = static_cast<SnClassDecl*>(pTarget);
        const std::string& baseName = pClassDecl->BaseName();
        if (baseName == kBuiltinDictTypeName) claimSize = 3;
        else if (baseName == kBuiltinListTypeName) claimSize = 2;
    }
    //Stage-2 ND: a rank>=2 token target lowers as one flat leaf walk —
    //the dims claim (rank slots) releases before the leaf loop's value
    //claim, so the band peak is (rank + 1 + deepest leaf). Rows are
    //structural (never emitted); only the leaves' depths count.
    if (pTarget && pTarget->Kind() == NK_ArrayTypeToken
        && static_cast<SnArrayTypeToken*>(pTarget)->Rank() >= 2) {
        const uint8_t rank =
            static_cast<SnArrayTypeToken*>(pTarget)->Rank();
        if (NDInitListHasExpressionRow(init,
                static_cast<uint8_t>(rank - 1)))
            return NDInitListDynamicPeakDepth(init, visited);
        //The leaf claim is [coords | value] = rank + 1 slots; nested
        //emissions inside a leaf value climb above it.
        uint16_t claim = static_cast<uint16_t>(rank + 1);
        uint16_t leafMax = NDLeafPeakDepth(init, visited);
        claim = static_cast<uint16_t>(claim + leafMax);
        return claim;
    }
    uint16_t maxChild = 0;
    for (auto& entry : init.Entries()) {
        //0.10.10: the Dict key stages into the claim's key slot, so its
        //nested depth rides the same claimSize — count it like the value.
        if (entry.pKey) {
            uint16_t cd = ExprPeakDepth(*entry.pKey, visited);
            if (cd > maxChild) maxChild = cd;
        }
        if (entry.pValue) {
            uint16_t cd = ExprPeakDepth(*entry.pValue, visited);
            if (cd > maxChild) maxChild = cd;
        }
    }
    return claimSize + maxChild;
}

//Stage-3 dynamic arm of the ND depth: the row-handle claim (one slot
//per entry) hosts every entry's full emission depth.
uint16_t VmBackend::NDInitListDynamicPeakDepth(SnInitListExpr& init,
    const std::unordered_set<SnFunction*>& visited) {
    uint16_t rowMax = 0;
    for (auto& entry : init.Entries()) {
        if (!entry.pValue) continue;
        uint16_t cd = ExprPeakDepth(*entry.pValue, visited);
        if (cd > rowMax) rowMax = cd;
    }
    return static_cast<uint16_t>(init.Entries().size() + rowMax);
}

//Deepest leaf-expression claim of an ND literal: rows are structural
//and emit nothing, so the walk descends through them without charging
//a row claim and takes the max ExprPeakDepth over the leaves.
uint16_t VmBackend::NDLeafPeakDepth(SnInitListExpr& init,
    const std::unordered_set<SnFunction*>& visited) {
    uint16_t deepest = 0;
    for (auto& entry : init.Entries()) {
        if (!entry.pValue)
            continue;
        uint16_t cd;
        if (SnInitListExpr::IsBracketForm(entry.pValue)) {
            cd = NDLeafPeakDepth(
                static_cast<SnInitListExpr&>(*entry.pValue), visited);
        } else {
            cd = ExprPeakDepth(*entry.pValue, visited);
        }
        if (cd > deepest) deepest = cd;
    }
    return deepest;
}

} //namespace nlang
