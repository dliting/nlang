/*---
    WalkersMaxArgs.cpp — 帧尺寸 walker 调用实参域：maxArgs 递归走查族
    （callParamBase 尺寸依据，服务 ComputeCallSlotStats）。
    从 WalkersStmt.cpp 拆出（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <unordered_set>

namespace nlang {
//MaxArgs walker statement domain: recurses over the function body and
//tracks the largest callParamBase claim into maxArgs.
void VmBackend::MaxArgsWalkStmt(SnStatement& stmt, uint16_t& maxArgs) {
    NodeKind kind = stmt.Kind();
    if (kind == NK_Paragraph) {
        for (auto& child : static_cast<SnParagraph&>(stmt).Statements())
            MaxArgsWalkStmt(child, maxArgs);
    } else if (kind == NK_ReturnStmt) {
        auto* r = static_cast<SnReturnStmt&>(stmt).Result();
        if (r) MaxArgsWalkExpr(*r, maxArgs);
    } else if (kind == NK_InvokeStmt) {
        MaxArgsWalkExpr(*static_cast<SnInvokeStmt&>(stmt).Expr(), maxArgs);
    } else if (kind == NK_AssignStmt) {
        MaxArgsAssignStmt(stmt, maxArgs);
    } else if (kind == NK_IfStmt) {
        auto& ifStmt = static_cast<SnIfStmt&>(stmt);
        MaxArgsWalkExpr(*ifStmt.Cond(), maxArgs);
        if (ifStmt.ThenStmt()) MaxArgsWalkStmt(*ifStmt.ThenStmt(), maxArgs);
        if (ifStmt.ElseStmt()) MaxArgsWalkStmt(*ifStmt.ElseStmt(), maxArgs);
    } else if (kind == NK_WhileStmt) {
        auto& w = static_cast<SnWhileStmt&>(stmt);
        MaxArgsWalkExpr(*w.Cond(), maxArgs);
        if (w.Body()) MaxArgsWalkStmt(*w.Body(), maxArgs);
    } else if (kind == NK_DoStmt) {
        auto& dw = static_cast<SnDoStmt&>(stmt);
        MaxArgsWalkExpr(*dw.Cond(), maxArgs);
        if (dw.Body()) MaxArgsWalkStmt(*dw.Body(), maxArgs);
    } else if (kind == NK_ForStmt) {
        MaxArgsForStmt(stmt, maxArgs);
    } else if (kind == NK_SwitchStmt) {
        MaxArgsSwitchStmt(stmt, maxArgs);
    } else if (kind == NK_ForeachStmt) {
        MaxArgsForeachStmt(stmt, maxArgs);
    } else if (kind == NK_AssertStmt) {
        MaxArgsWalkExpr(*static_cast<SnAssertStmt&>(stmt).Cond(), maxArgs);
    } else if (kind == NK_TryStmt) {
        MaxArgsTryStmt(stmt, maxArgs);
    } else if (kind == NK_SuperCallStmt) {
        MaxArgsSuperCallStmt(stmt, maxArgs);
    } else if (kind == NK_ThrowStmt) {
        auto* e = static_cast<SnThrowStmt&>(stmt).Expr();
        if (e) MaxArgsWalkExpr(*e, maxArgs);
    } else if (kind == NK_CompoundAssignStmt) {
        MaxArgsCompoundAssignStmt(stmt, maxArgs);
    } else if (kind == NK_SubscriptAssignStmt) {
        MaxArgsSubscriptAssignStmt(stmt, maxArgs);
    }
}

//MaxArgs walker expression domain: recurses into child expressions for
//nested calls; isMethodContext as in ExprPeakDepth.
void VmBackend::MaxArgsWalkExpr(SnExpression& expr, uint16_t& maxArgs,
    bool isMethodContext) {
    if (expr.Kind() == NK_InvokeExpr) {
        MaxArgsInvokeExpr(expr, maxArgs, isMethodContext);
        return;
    }
    //Recurse into children for nested calls.
    if (expr.Kind() == NK_BinaryExpr) {
        auto& bin = static_cast<SnBinaryExpr&>(expr);
        MaxArgsWalkExpr(*bin.Left(), maxArgs);
        if (bin.Right()) MaxArgsWalkExpr(*bin.Right(), maxArgs);
    } else if (expr.Kind() == NK_CastExpr) {
        MaxArgsWalkExpr(*static_cast<SnCastExpr&>(expr).Source(), maxArgs);
    } else if (expr.Kind() == NK_AsExpr) {
        MaxArgsWalkExpr(*static_cast<SnAsExpr&>(expr).Operand(), maxArgs);
    } else if (expr.Kind() == NK_NamedArgExpr) {
        MaxArgsWalkExpr(*static_cast<SnNamedArgExpr&>(expr).Inner(), maxArgs);
    } else if (expr.Kind() == NK_OutArgExpr) {
        //Phase 9e: out arg — walk the inner identifier.
        MaxArgsWalkExpr(*static_cast<SnOutArgExpr&>(expr).Inner(), maxArgs);
    } else if (expr.Kind() == NK_SubscriptExpr) {
        MaxArgsSubscriptExpr(expr, maxArgs);
    } else if (expr.Kind() == NK_MemberExpr) {
        MaxArgsMemberExpr(expr, maxArgs);
    } else if (expr.Kind() == NK_NewExpr) {
        MaxArgsNewExpr(expr, maxArgs);
    } else if (expr.Kind() == NK_NewArrayExpr) {
        MaxArgsWalkExpr(*static_cast<SnNewArrayExpr&>(expr).Size(), maxArgs);
    } else if (expr.Kind() == NK_InitListExpr) {
        MaxArgsInitListExpr(expr, maxArgs);
    }
}

//MaxArgs walker, NK_AssignStmt arm.
void VmBackend::MaxArgsAssignStmt(SnStatement& stmt, uint16_t& maxArgs) {
    //Walk the Left() lvalue too — `li[i].f = v` receivers emit
    //a synthetic get() call needing 2 callParamBase slots.
    auto& as = static_cast<SnAssignStmt&>(stmt);
    auto* l = as.Left();
    if (l) MaxArgsWalkExpr(*l, maxArgs);
    auto* v = as.Right();
    if (v) MaxArgsWalkExpr(*v, maxArgs);
}

//MaxArgs walker, NK_ForStmt arm.
void VmBackend::MaxArgsForStmt(SnStatement& stmt, uint16_t& maxArgs) {
    auto& f = static_cast<SnForStmt&>(stmt);
    if (f.Init()) MaxArgsWalkStmt(*f.Init(), maxArgs);
    //Round-10: decomposed init assigns (local-decl split) may
    //contain calls — mirror the codegen's InitExtras loop.
    for (auto* pExtra : f.InitExtras()) MaxArgsWalkStmt(*pExtra, maxArgs);
    if (f.Cond()) MaxArgsWalkExpr(*f.Cond(), maxArgs);
    if (f.Fini()) MaxArgsWalkStmt(*f.Fini(), maxArgs);
    if (f.Body()) MaxArgsWalkStmt(*f.Body(), maxArgs);
}

//MaxArgs walker, NK_SwitchStmt arm.
void VmBackend::MaxArgsSwitchStmt(SnStatement& stmt, uint16_t& maxArgs) {
    auto& sw = static_cast<SnSwitchStmt&>(stmt);
    MaxArgsWalkExpr(*sw.Cond(), maxArgs);
    for (auto* c : sw.Cases()) {
        //SnCaseClause inherits SyntaxNode, not SnStatement.
        for (auto* pLabel : c->Labels())
            MaxArgsWalkExpr(*pLabel, maxArgs);
        if (c->Body()) {
            for (auto& s : c->Body()->Statements())
                MaxArgsWalkStmt(s, maxArgs);
        }
    }
    //Round-10: default clause body — emitted after the cases.
    if (sw.Default()) MaxArgsWalkStmt(*sw.Default(), maxArgs);
}

//MaxArgs walker, NK_ForeachStmt arm.
void VmBackend::MaxArgsForeachStmt(SnStatement& stmt, uint16_t& maxArgs) {
    auto& fe = static_cast<SnForeachStmt&>(stmt);
    //Round-11: the List/Dict expansion body-prelude lowers to a
    //synthetic get(i) call that bulk-writes {this, index} into
    //callParamBase — reserve them (SubscriptExpr precedent).
    if (maxArgs < 2) maxArgs = 2;
    MaxArgsWalkExpr(*fe.Iterable(), maxArgs);
    if (fe.Body()) MaxArgsWalkStmt(*fe.Body(), maxArgs);
}

//MaxArgs walker, NK_TryStmt arm.
void VmBackend::MaxArgsTryStmt(SnStatement& stmt, uint16_t& maxArgs) {
    auto& ts = static_cast<SnTryStmt&>(stmt);
    if (ts.TryBody()) MaxArgsWalkStmt(*ts.TryBody(), maxArgs);
    for (auto* c : ts.Catches()) {
        if (c->Body()) MaxArgsWalkStmt(*c->Body(), maxArgs);
    }
    if (ts.FinallyBody()) MaxArgsWalkStmt(*ts.FinallyBody(), maxArgs);
}

//MaxArgs walker, NK_SuperCallStmt arm.
void VmBackend::MaxArgsSuperCallStmt(SnStatement& stmt, uint16_t& maxArgs) {
    //super(args) claims 1 (this) + argc slots at call time.
    auto& sc = static_cast<SnSuperCallStmt&>(stmt);
    uint16_t claimSize = static_cast<uint16_t>(1 + sc.Args().size());
    if (claimSize > maxArgs) maxArgs = claimSize;
    for (auto* arg : sc.Args())
        MaxArgsWalkExpr(*arg, maxArgs);
}

//MaxArgs walker, NK_CompoundAssignStmt arm.
void VmBackend::MaxArgsCompoundAssignStmt(SnStatement& stmt, uint16_t& maxArgs) {
    //Walk Left() receiver too — `mk().x += 1` hides a call
    //needing callParamBase slots (C1-family asymmetry).
    auto& ca = static_cast<SnCompoundAssignStmt&>(stmt);
    if (ca.Left()) MaxArgsWalkExpr(*ca.Left(), maxArgs);
    if (ca.Right()) MaxArgsWalkExpr(*ca.Right(), maxArgs);
}

//MaxArgs walker, NK_SubscriptAssignStmt arm.
void VmBackend::MaxArgsSubscriptAssignStmt(SnStatement& stmt, uint16_t& maxArgs) {
    auto& sa = static_cast<SnSubscriptAssignStmt&>(stmt);
    //List/Dict subscript store lowers to a set() call that
    //bulk-copies 3 slots into callParamBase — reserve them.
    //Array stores read their claim slots directly (StoreElement
    //takes explicit operand offsets) and touch no call params.
    if (IsContainerSubscript(*sa.Array()) && maxArgs < 3)
        maxArgs = 3;
    //Phase 10 audit C1: walk the array base too — a call in the
    //base (`makeArr(...)[0] = v`) needs its arg slots reserved or
    //the bulk-copy overflows callParamBase into evalArea.
    //StmtPeakDepth already walked it; this closes the asymmetry.
    MaxArgsWalkExpr(*sa.Array(), maxArgs);
    MaxArgsWalkExpr(*sa.Index(), maxArgs);
    MaxArgsWalkExpr(*sa.Value(), maxArgs);
}

//MaxArgs walker, NK_InvokeExpr arm.
void VmBackend::MaxArgsInvokeExpr(SnExpression& expr, uint16_t& maxArgs,
    bool isMethodContext) {
    auto& invoke = static_cast<SnInvokeExpr&>(expr);
    auto* callee = invoke.Callee();
    size_t formalCount = callee ? callee->Params().size() : 0;
    if (!callee) { for (auto& p : invoke.Params()) ++formalCount; }
    //Method call detection: trust isMethodContext flag (set
    //when Inner of MemberExpr) OR callee->Parent() is class.
    //Built-in method calls have callee=null, so the flag is
    //the only reliable signal.
    bool isMethod = isMethodContext || (callee && callee->Parent()
        && (callee->Parent()->Kind() == NK_ClassDecl
            || callee->Parent()->Kind() == NK_StructDecl
            || callee->Parent()->Kind() == NK_InterfaceDecl));
    size_t slotBase = isMethod ? 1 : 0;
    uint16_t claimSize = static_cast<uint16_t>(
        formalCount + slotBase);
    //Phase 13: delegate invoke callee scratch slot — mirror
    //ExprPeakDepth (over-reservation, safe direction).
    if (!callee && IsDelegateInvoke(invoke))
        claimSize = static_cast<uint16_t>(claimSize + 1);
    //Phase 11 Step 3 symmetry (review BLOCKER): a built-in
    // string method with a staged trailing argument (the
    //1-arg substring form) bulk-copies 1+maxArgs slots into
    //callParamBase — reserve the same count here from the
    //same table field ExprPeakDepth/codegen consume, or the
    //i=2 write lands on evalArea slot 0 and clobbers a live
    //parked operand (`a + s.substring(1)` compared garbage).
    //Name-based overreserve for a free function sharing the
    //name is the safe direction (frame bytes only).
    if (!callee) {
        const StringMethodEntry* pm =
            FindStringMethod(invoke.CalleeName());
        if (pm && pm->trailingDefault == STD_ReceiverLength
            && formalCount < pm->maxArgs)
            claimSize = static_cast<uint16_t>(
                claimSize + (pm->maxArgs - formalCount));
    }
    if (claimSize > maxArgs) maxArgs = claimSize;
    //Also walk the invoke's own params for nested calls.
    for (auto& param : invoke.Params())
        MaxArgsWalkExpr(param, maxArgs);
}

//MaxArgs walker, NK_SubscriptExpr arm.
void VmBackend::MaxArgsSubscriptExpr(SnExpression& expr, uint16_t& maxArgs) {
    auto& sub = static_cast<SnSubscriptExpr&>(expr);
    //List/Dict subscript lowers to a synthetic get() call that
    //bulk-copies 2 slots (this + index) into callParamBase —
    //mirror the codegen so maxArgs never under-reserves the
    //region (a lone `li[0]` with no other calls would size
    //callParamBase at 1 and the get() would overflow it).
    if (IsContainerSubscript(*sub.Array()) && maxArgs < 2)
        maxArgs = 2;
    MaxArgsWalkExpr(*sub.Array(), maxArgs);
    MaxArgsWalkExpr(*sub.Index(), maxArgs);
}

//MaxArgs walker, NK_MemberExpr arm.
void VmBackend::MaxArgsMemberExpr(SnExpression& expr, uint16_t& maxArgs) {
    auto& member = static_cast<SnMemberExpr&>(expr);
    MaxArgsWalkExpr(*member.Outer(), maxArgs, false);
    //If Inner is InvokeExpr, pass method-context flag so
    //slot 0 is reserved for `this` — EXCEPT delegate invokes
    //(a func-typed field): their args are staged without this
    //and the invoke branch adds the callee scratch instead
    //(mirror ExprPeakDepth's MemberExpr branch).
    if (member.Inner()
        && member.Inner()->Kind() == NK_InvokeExpr) {
        auto& innerInvoke =
            static_cast<SnInvokeExpr&>(*member.Inner());
        MaxArgsWalkExpr(*member.Inner(), maxArgs,
            !IsDelegateInvoke(innerInvoke));
    } else {
        MaxArgsWalkExpr(*member.Inner(), maxArgs, false);
    }
}

//MaxArgs walker, NK_NewExpr arm.
void VmBackend::MaxArgsNewExpr(SnExpression& expr, uint16_t& maxArgs) {
    //claimSize for ctor call = 1 (this) + argCount. Skip the
    //class-name child by identity (Args() view includes it —
    //see the NewExpr codegen handler); mirror codegen exactly.
    auto& newExpr = static_cast<SnNewExpr&>(expr);
    size_t argCount = 0;
    for (auto& arg : newExpr.Args()) {
        if (&arg == newExpr.ClassName()) continue;
        ++argCount;
    }
    uint16_t claimSize = static_cast<uint16_t>(1 + argCount);
    if (claimSize > maxArgs) maxArgs = claimSize;
    for (auto& arg : newExpr.Args()) {
        if (&arg == newExpr.ClassName()) continue;
        MaxArgsWalkExpr(arg, maxArgs);
    }
}

//MaxArgs walker, NK_InitListExpr arm.
void VmBackend::MaxArgsInitListExpr(SnExpression& expr, uint16_t& maxArgs) {
    //Phase 9c follow-up: collection inits emit implicit calls:
    //  - Dict: OP_CallMethod "set" with 3 slots (this, key, value)
    //  - List: OP_CallMethod "add" with 2 slots (this, value)
    //Track for maxArgs so callParamBase is sized correctly when
    //the legacy floor (8) is eventually removed.
    auto& init = static_cast<SnInitListExpr&>(expr);
    SnField* pTarget = init.EvalDataType();
    if (pTarget && pTarget->Kind() == NK_ClassDecl) {
        auto* pClassDecl = static_cast<SnClassDecl*>(pTarget);
        const std::string& bn = pClassDecl->BaseName();
        if (bn == kBuiltinDictTypeName && 3 > maxArgs) maxArgs = 3;
        else if (bn == kBuiltinListTypeName && 2 > maxArgs) maxArgs = 2;
    }
    for (auto& entry : init.Entries())
        if (entry.pValue) MaxArgsWalkExpr(*entry.pValue, maxArgs);
}
} //namespace nlang
