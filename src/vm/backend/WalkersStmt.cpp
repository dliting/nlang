/*---
    WalkersStmt.cpp — 帧尺寸 walker 语句域：
    语句求值峰值深度 + 调用槽统计入口（MaxArgsWalker）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化：
    walker 与共享谓词提升为 VmBackend 静态成员，见 VmBackend.h）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <unordered_set>

namespace nlang {

uint16_t VmBackend::StmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    NodeKind kind = stmt.Kind();
    if (kind == NK_Paragraph)
        return ParagraphPeakDepth(stmt, visited);
    if (kind == NK_ReturnStmt)
        return ReturnStmtPeakDepth(stmt, visited);
    if (kind == NK_InvokeStmt) {
        auto& invoke = static_cast<SnInvokeStmt&>(stmt);
        //Claim 1 stages the discarded result (codegen, round-9).
        return 1 + ExprPeakDepth(*invoke.Expr(), visited);
    }
    if (kind == NK_LocalDeclStmt) {
        auto& decl = static_cast<SnLocalDeclStmt&>(stmt);
        //Initializer is handled by a subsequent AssignStmt.
        return 0;
    }
    if (kind == NK_AssignStmt)
        return AssignStmtPeakDepth(stmt, visited);
    if (kind == NK_IfStmt)
        return IfStmtPeakDepth(stmt, visited);
    if (kind == NK_WhileStmt)
        return WhileStmtPeakDepth(stmt, visited);
    if (kind == NK_DoStmt)
        return DoStmtPeakDepth(stmt, visited);
    if (kind == NK_ForStmt)
        return ForStmtPeakDepth(stmt, visited);
    if (kind == NK_SwitchStmt)
        return SwitchStmtPeakDepth(stmt, visited);
    if (kind == NK_ForeachStmt)
        return ForeachStmtPeakDepth(stmt, visited);
    if (kind == NK_BreakStmt || kind == NK_ContinueStmt) {
        return 0;
    }
    if (kind == NK_AssertStmt)
        return AssertStmtPeakDepth(stmt, visited);
    if (kind == NK_TryStmt)
        return TryStmtPeakDepth(stmt, visited);
    if (kind == NK_SuperCallStmt)
        return SuperCallStmtPeakDepth(stmt, visited);
    if (kind == NK_ThrowStmt)
        return ThrowStmtPeakDepth(stmt, visited);
    if (kind == NK_CompoundAssignStmt)
        return CompoundAssignStmtPeakDepth(stmt, visited);
    if (kind == NK_SubscriptAssignStmt)
        return SubscriptAssignStmtPeakDepth(stmt, visited);
    return 0;
}

//NK_Paragraph arm of StmtPeakDepth.
uint16_t VmBackend::ParagraphPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& para = static_cast<SnParagraph&>(stmt);
    uint16_t d = 0;
    for (auto& child : para.Statements()) {
        uint16_t cd = StmtPeakDepth(child, visited);
        if (cd > d) d = cd;
    }
    return d;
}

//NK_ReturnStmt arm of StmtPeakDepth.
uint16_t VmBackend::ReturnStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& ret = static_cast<SnReturnStmt&>(stmt);
    return ret.Result() ? ExprPeakDepth(*ret.Result(), visited) : 0;
}

//NK_AssignStmt arm of StmtPeakDepth.
uint16_t VmBackend::AssignStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& assign = static_cast<SnAssignStmt&>(stmt);
    //Walk the Left() lvalue too: a member-assign whose receiver is a
    //List/Dict subscript (`li[i].f = v`) emits a synthetic get() call
    //claiming 2 evalArea slots — under-reserving evalArea corrupts the
    //frame (walker-symmetry discipline, 5th instance).
    uint16_t l = assign.Left()
        ? ExprPeakDepth(*assign.Left(), visited) : 0;
    uint16_t r = assign.Right()
        ? ExprPeakDepth(*assign.Right(), visited) : 0;
    //Member targets park their staging in EvalAreaClaims (round-4):
    //array member-write (`arr[i].f = v`) claims 3 [array, index,
    //value]; plain member (`obj.f = v`), struct-to-struct and
    //container member-write (`li[i].f = v`) claim 2 [receiver, value].
    //The walker cannot cheaply separate the shapes, so claim a uniform
    //3 — over-reserving is the safe direction (container receivers
    //also count their get() claim inside the SubscriptExpr case).
    uint16_t claim = 0;
    if (assign.Left() && assign.Left()->Kind() == NK_MemberExpr)
        claim = 3;
    //Identifier targets (plain local `x = rhs`) stage the RHS in an
    //EvalAreaClaim(1) before copying to the destination (round-7 —
    //aliasing family). The struct branch (CopyStruct staging) and the
    //implicit this-field branch use temps only after their last
    //nested emission, needing no claim — over-reserve is safe.
    else if (assign.Left()
        && assign.Left()->Kind() == NK_IdentifierExpr)
        claim = 1;
    uint16_t m = l > r ? l : r;
    return claim + m;
}

//NK_IfStmt arm of StmtPeakDepth.
uint16_t VmBackend::IfStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& ifStmt = static_cast<SnIfStmt&>(stmt);
    //Claim 1 stages the condition (codegen, round-9).
    uint16_t d = 1 + ExprPeakDepth(*ifStmt.Cond(), visited);
    if (ifStmt.ThenStmt()) {
        uint16_t td = StmtPeakDepth(*ifStmt.ThenStmt(), visited);
        if (td > d) d = td;
    }
    if (ifStmt.ElseStmt()) {
        uint16_t ed = StmtPeakDepth(*ifStmt.ElseStmt(), visited);
        if (ed > d) d = ed;
    }
    return d;
}

//NK_WhileStmt arm of StmtPeakDepth.
uint16_t VmBackend::WhileStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& whileStmt = static_cast<SnWhileStmt&>(stmt);
    //Claim 1 stages the condition (codegen, round-9).
    uint16_t d = 1 + ExprPeakDepth(*whileStmt.Cond(), visited);
    if (whileStmt.Body()) {
        uint16_t bd = StmtPeakDepth(*whileStmt.Body(), visited);
        if (bd > d) d = bd;
    }
    return d;
}

//NK_DoStmt arm of StmtPeakDepth.
uint16_t VmBackend::DoStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& dw = static_cast<SnDoStmt&>(stmt);
    //Claim 1 stages the condition (codegen, round-9).
    uint16_t d = 1 + ExprPeakDepth(*dw.Cond(), visited);
    if (dw.Body()) {
        uint16_t bd = StmtPeakDepth(*dw.Body(), visited);
        if (bd > d) d = bd;
    }
    return d;
}

//NK_ForStmt arm of StmtPeakDepth.
uint16_t VmBackend::ForStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& forStmt = static_cast<SnForStmt&>(stmt);
    uint16_t d = 0;
    if (forStmt.Init()) { uint16_t id = StmtPeakDepth(*forStmt.Init(), visited); if (id > d) d = id; }
    //Round-10: a for-init local decl decomposes into AssignStmts
    //(InitExtras) that the codegen emits after Init() — mirror them or
    //a staged initializer drifts past the reserved frame.
    for (auto* pExtra : forStmt.InitExtras()) {
        uint16_t ed = StmtPeakDepth(*pExtra, visited);
        if (ed > d) d = ed;
    }
    //Claim 1 stages the condition (codegen, round-9).
    if (forStmt.Cond()) { uint16_t cd = 1 + ExprPeakDepth(*forStmt.Cond(), visited); if (cd > d) d = cd; }
    if (forStmt.Fini()) { uint16_t fd = StmtPeakDepth(*forStmt.Fini(), visited); if (fd > d) d = fd; }
    if (forStmt.Body()) { uint16_t bd = StmtPeakDepth(*forStmt.Body(), visited); if (bd > d) d = bd; }
    return d;
}

//NK_SwitchStmt arm of StmtPeakDepth.
uint16_t VmBackend::SwitchStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& sw = static_cast<SnSwitchStmt&>(stmt);
    uint16_t d = ExprPeakDepth(*sw.Cond(), visited);
    for (auto* c : sw.Cases()) {
        //SnCaseClause inherits SyntaxNode, not SnStatement — inline the walk.
        //Claim 1 stages each case-label (codegen, round-9; labels are
        //sequential so one claim's worth suffices — Phase 12 multi-label
        //keeps the same reasoning: one label claims at a time).
        for (auto* pLabel : c->Labels()) {
            uint16_t cd = 1 + ExprPeakDepth(*pLabel, visited);
            if (cd > d) d = cd;
        }
        if (c->Body()) {
            for (auto& s : c->Body()->Statements()) {
                uint16_t sd = StmtPeakDepth(s, visited);
                if (sd > d) d = sd;
            }
        }
    }
    //Round-10: the default clause body is emitted after the cases —
    //walk it like a case body (SnParagraph, not SnStatement).
    if (sw.Default()) {
        for (auto& s : sw.Default()->Statements()) {
            uint16_t sd = StmtPeakDepth(s, visited);
            if (sd > d) d = sd;
        }
    }
    return d;
}

//NK_ForeachStmt arm of StmtPeakDepth.
uint16_t VmBackend::ForeachStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& fe = static_cast<SnForeachStmt&>(stmt);
    uint16_t d = ExprPeakDepth(*fe.Iterable(), visited);
    if (fe.Body()) {
        uint16_t bd = StmtPeakDepth(*fe.Body(), visited);
        if (bd > d) d = bd;
    }
    return d;
}

//NK_AssertStmt arm of StmtPeakDepth.
uint16_t VmBackend::AssertStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& as = static_cast<SnAssertStmt&>(stmt);
    //Claim 1 stages the condition (codegen, round-9).
    return 1 + ExprPeakDepth(*as.Cond(), visited);
}

//NK_TryStmt arm of StmtPeakDepth.
uint16_t VmBackend::TryStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& ts = static_cast<SnTryStmt&>(stmt);
    uint16_t d = 0;
    if (ts.TryBody()) { uint16_t bd = StmtPeakDepth(*ts.TryBody(), visited); if (bd > d) d = bd; }
    for (auto* c : ts.Catches()) {
        if (c->Body()) { uint16_t bd = StmtPeakDepth(*c->Body(), visited); if (bd > d) d = bd; }
    }
    if (ts.FinallyBody()) { uint16_t fd = StmtPeakDepth(*ts.FinallyBody(), visited); if (fd > d) d = fd; }
    return d;
}

//NK_SuperCallStmt arm of StmtPeakDepth.
uint16_t VmBackend::SuperCallStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& sc = static_cast<SnSuperCallStmt&>(stmt);
    //Mirror the codegen's EvalAreaClaim(1 + args) — same claimSize
    //pattern as NK_NewExpr in ExprPeakDepth. peakDepth without the
    //claimSize under-sizes evalArea and super-arg staging writes past
    //the frame (heap-buffer-overflow, deterministic in codegen).
    uint16_t claimSize = static_cast<uint16_t>(1 + sc.Args().size());
    uint16_t d = 0;
    for (auto* arg : sc.Args()) {
        uint16_t ad = ExprPeakDepth(*arg, visited);
        if (ad > d) d = ad;
    }
    return claimSize + d;
}

//NK_ThrowStmt arm of StmtPeakDepth.
uint16_t VmBackend::ThrowStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& th = static_cast<SnThrowStmt&>(stmt);
    //Claim 1 stages the thrown expression (codegen, round-9).
    return th.Expr() ? 1 + ExprPeakDepth(*th.Expr(), visited) : 0;
}

//NK_CompoundAssignStmt arm of StmtPeakDepth.
uint16_t VmBackend::CompoundAssignStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& ca = static_cast<SnCompoundAssignStmt&>(stmt);
    //Member targets (`obj.f += v` and implicit `this.f += v`) park
    //receiver/old-value/RHS in an EvalAreaClaim(3); the walker cannot
    //distinguish the implicit-this shape (Left is a bare identifier
    //resolving to a this-field at codegen time), so claim 3
    //unconditionally — local targets stage in tempSlot2 and merely
    //over-reserve (the safe direction). Walk the Left() receiver too:
    //`mk().x += 1` hides a call in the receiver.
    uint16_t claim = 3;
    uint16_t l = ca.Left() ? ExprPeakDepth(*ca.Left(), visited) : 0;
    uint16_t r = ca.Right() ? ExprPeakDepth(*ca.Right(), visited) : 0;
    return claim + (l > r ? l : r);
}

//NK_SubscriptAssignStmt arm of StmtPeakDepth.
uint16_t VmBackend::SubscriptAssignStmtPeakDepth(SnStatement& stmt,
    const std::unordered_set<SnFunction*>& visited) {
    auto& sa = static_cast<SnSubscriptAssignStmt&>(stmt);
    //Both paths run inside an EvalAreaClaim(3) [receiver + index +
    //value] — container lowers to a set() call, array to a direct
    //StoreElement; either way the walker must mirror the codegen's
    //claim. walker-symmetry discipline, 4th instance (after
    //callparambase-clobber #2 and super() #3).
    uint16_t claim = 3;
    uint16_t d = ExprPeakDepth(*sa.Index(), visited);
    uint16_t v = ExprPeakDepth(*sa.Value(), visited);
    uint16_t a = ExprPeakDepth(*sa.Array(), visited);
    uint16_t m = d > v ? d : v;
    if (a > m) m = a;
    return claim + m;
}

VmBackend::CallSlotStats VmBackend::ComputeCallSlotStats(SnFunction& sn) {
    CallSlotStats stats{1, 1};  //min 1 slot each
    std::unordered_set<SnFunction*> visited;
    visited.insert(&sn);

    //Walk body for peakDepth.
    if (sn.Body()) {
        for (auto& stmt : sn.Body()->Statements()) {
            uint16_t d = StmtPeakDepth(stmt, visited);
            if (d > stats.peakDepth) stats.peakDepth = d;
        }
    }

    //Walk body for maxArgs (max callee formal count + slotBase).
    //Reuse a simple recursive helper.
    uint16_t maxArgs = 1;
    if (sn.Body()) {
        for (auto& stmt : sn.Body()->Statements())
            MaxArgsWalkStmt(stmt, maxArgs);
    }
    stats.maxArgs = maxArgs;
    return stats;
}

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
    //(a Func-typed field): their args are staged without this
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
        if (bn == "Dict" && 3 > maxArgs) maxArgs = 3;
        else if (bn == "List" && 2 > maxArgs) maxArgs = 2;
    }
    for (auto& entry : init.Entries())
        if (entry.pValue) MaxArgsWalkExpr(*entry.pValue, maxArgs);
}

} //namespace nlang
