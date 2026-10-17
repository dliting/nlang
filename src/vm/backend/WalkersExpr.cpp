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
//a func-typed value (local/param/field) rather than an SnFunction. Shared
//by codegen and both frame-size walkers so their claim shapes stay in
//lockstep (walker symmetry, 6th instance).
bool VmBackend::IsDelegateInvoke(const SnInvokeExpr& invoke)
{
    return invoke.Field() != nullptr
        && invoke.Field()->Kind() != NK_Function;
}

uint16_t VmBackend::ExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited,
    bool isMethodContext) {
    NodeKind kind = expr.Kind();
    if (kind == NK_InvokeExpr)
        return InvokeExprPeakDepth(expr, visited, isMethodContext);
    //Non-invoke expressions: recurse into children.
    //UnaryExpr: NLang uses NK_BinaryExpr for both binary and unary.
    //Unary ops (OP_Neg, OP_LogicalNot) have Right()==nullptr.
    if (kind == NK_BinaryExpr)
        return BinaryExprPeakDepth(expr, visited);
    //CastExpr
    if (kind == NK_CastExpr) {
        auto& cast = static_cast<SnCastExpr&>(expr);
        return ExprPeakDepth(*cast.Source(), visited);
    }
    //AsExpr (expr as T)
    if (kind == NK_AsExpr) {
        auto& as = static_cast<SnAsExpr&>(expr);
        return ExprPeakDepth(*as.Operand(), visited);
    }
    //NamedArgExpr
    if (kind == NK_NamedArgExpr) {
        auto& named = static_cast<SnNamedArgExpr&>(expr);
        return ExprPeakDepth(*named.Inner(), visited);
    }
    //OutArgExpr (Phase 9e) — transparent like NamedArgExpr: its inner
    //identifier evaluates into the claimed binding slot.
    if (kind == NK_OutArgExpr) {
        auto& out = static_cast<SnOutArgExpr&>(expr);
        return ExprPeakDepth(*out.Inner(), visited);
    }
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
    //Leaf expressions (LiteralExpr, IdentifierExpr, ThisExpr, etc.)
    return 0;
}

//NK_InvokeExpr arm of ExprPeakDepth.
uint16_t VmBackend::InvokeExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited, bool isMethodContext) {
    auto& invoke = static_cast<SnInvokeExpr&>(expr);
    //0.9.0: a ctor-carrying invoke shares the NewExpr claim shape.
    if (invoke.CtorClass())
        return CtorInvokePeakDepth(invoke, visited);
    auto* callee = invoke.Callee();
    size_t formalCount = callee ? callee->Params().size() : 0;
    if (!callee) { for (auto& p : invoke.Params()) ++formalCount; }
    //Method call (slotBase=1) when caller signals method context,
    //OR when callee is resolved to a method (Parent is class/struct/
    //interface). The method-context flag is authoritative for
    //built-in method calls where callee is null.
    bool isMethod = isMethodContext || (callee && callee->Parent()
        && (callee->Parent()->Kind() == NK_ClassDecl
            || callee->Parent()->Kind() == NK_StructDecl
            || callee->Parent()->Kind() == NK_InterfaceDecl));
    size_t slotBase = isMethod ? 1 : 0;
    size_t claimSize = formalCount + slotBase;
    //Phase 13: a delegate invoke materializes its callee handle into
    //one scratch slot. The claims are sequential (args first, then
    //callee), so this is over-reservation — the safe direction.
    if (!callee && VmBackend::IsDelegateInvoke(invoke))
        claimSize += 1;

    //Peak depth of argument sub-expressions.
    uint16_t argDepth = 0;
    for (auto& param : invoke.Params()) {
        uint16_t d = ExprPeakDepth(param, visited);
        if (d > argDepth) argDepth = d;
    }

    //Peak depth of callee's default expressions (evaluated in caller frame).
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
    for (auto& param : invoke.Params()) {
        ++argCount;
        uint16_t ad = ExprPeakDepth(param, visited, false);
        if (ad > argDepth) argDepth = ad;
    }
    SnFunction* pCtorFn = FindCtorFunction(invoke.CtorClass());
    const size_t totalFormals = pCtorFn
        ? pCtorFn->Params().size() : argCount;
    return static_cast<uint16_t>(1 + totalFormals)
        + CtorDefaultExprDepth(pCtorFn, visited, argDepth);
}

//NK_BinaryExpr arm of ExprPeakDepth.
uint16_t VmBackend::BinaryExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    auto& bin = static_cast<SnBinaryExpr&>(expr);
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

//NK_SubscriptExpr arm of ExprPeakDepth.
uint16_t VmBackend::SubscriptExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    auto& sub = static_cast<SnSubscriptExpr&>(expr);
    //0.9.0: a sized-array creation twin emits through the new-array
    //path — the size stages in an evalArea claim(1) and the base names
    //a type (no emission). Walker symmetry with NewArrayExprPeakDepth.
    if (sub.NewArrayElem())
        return 1 + ExprPeakDepth(*sub.Index(), visited);
    uint16_t a = ExprPeakDepth(*sub.Array(), visited);
    uint16_t i = ExprPeakDepth(*sub.Index(), visited);
    //Both shapes claim 2 evalArea slots (receiver + index): container
    //lowers to a get() call, array reads park receiver/index in the
    //claim directly (Phase 10 audit round-3 — was a 4-deep PickTempSlot
    //chain that wrapped and clobbered at nesting depth 5).
    uint16_t claim = 2;
    uint16_t m = (a > i ? a : i);
    return claim + m;
}

//NK_MemberExpr arm of ExprPeakDepth (field access: outer.inner).
uint16_t VmBackend::MemberExprPeakDepth(SnExpression& expr,
    const std::unordered_set<SnFunction*>& visited) {
    auto& member = static_cast<SnMemberExpr&>(expr);
    uint16_t d = ExprPeakDepth(*member.Outer(), visited, false);
    //If Inner is an InvokeExpr, this is a method call shape — pass
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
    //table field codegen consumes, or a 1-arg substring call would
    //clobber a slot above the walker-shaped frame. Overreserving for
    //a user method that happens to share the name is safe (finalize
    //only asserts observed <= walker).
    if (member.Inner() && member.Inner()->Kind() == NK_InvokeExpr) {
        auto& inv = static_cast<SnInvokeExpr&>(*member.Inner());
        const StringMethodEntry* pm = FindStringMethod(inv.CalleeName());
        if (pm && pm->trailingDefault == STD_ReceiverLength) {
            size_t actual = 0;
            for (auto& p : inv.Params()) ++actual;
            if (actual < pm->maxArgs)
                id = static_cast<uint16_t>(
                    id + (pm->maxArgs - actual));
        }
    }
    return d > id ? d : id;
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
    SnFunction* pCtorFn = FindCtorFunction(newExpr.ClassDecl());
    const size_t totalFormals = pCtorFn
        ? pCtorFn->Params().size() : argCount;
    uint16_t claimSize = static_cast<uint16_t>(1 + totalFormals);
    uint16_t d = 0;
    for (auto& arg : newExpr.Args()) {
        //Skip the class-name child (Args() view includes it — see the
        //NewExpr codegen handler); mirror codegen exactly.
        if (&arg == newExpr.ClassName()) continue;
        uint16_t ad = ExprPeakDepth(arg, visited, false);
        if (ad > d) d = ad;
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
    uint16_t maxChild = 0;
    for (auto& entry : init.Entries()) {
        if (entry.pValue) {
            uint16_t cd = ExprPeakDepth(*entry.pValue, visited);
            if (cd > maxChild) maxChild = cd;
        }
    }
    return claimSize + maxChild;
}

} //namespace nlang
