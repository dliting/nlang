/*---
    WalkersStmt.cpp — 帧尺寸 walker 语句域：
    语句求值峰值深度族 + 调用槽统计入口（maxArgs 走查族在 WalkersMaxArgs.cpp）。
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
    //0.8.4: a bare subscript LHS (arr[i] += v) parks base/index/old/RHS
    //in an EvalAreaClaim(4). Base and index are emitted directly into
    //the claim slots (no read-path claim of their own — the get()/
    //set() calls ride callParamBase, and the array path is direct
    //element ops), so walk them separately rather than through
    //ExprPeakDepth(*Left()), which would add the read path's own
    //claim(2) on top.
    if (ca.Left() && ca.Left()->Kind() == NK_SubscriptExpr) {
        auto& sub = static_cast<SnSubscriptExpr&>(*ca.Left());
        uint16_t b = ExprPeakDepth(*sub.Array(), visited);
        uint16_t i = ExprPeakDepth(*sub.Index(), visited);
        uint16_t r = ca.Right()
            ? ExprPeakDepth(*ca.Right(), visited) : 0;
        uint16_t m = b > i ? b : i;
        if (r > m) m = r;
        return 4 + m;
    }
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
} //namespace nlang
