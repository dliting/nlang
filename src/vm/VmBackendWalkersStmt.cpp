/*---
    VmBackendWalkersStmt.cpp — 帧尺寸 walker 语句域：
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
    if (kind == NK_Paragraph) {
        auto& para = static_cast<SnParagraph&>(stmt);
        uint16_t d = 0;
        for (auto& child : para.Statements()) {
            uint16_t cd = StmtPeakDepth(child, visited);
            if (cd > d) d = cd;
        }
        return d;
    }
    if (kind == NK_ReturnStmt) {
        auto& ret = static_cast<SnReturnStmt&>(stmt);
        return ret.Result() ? ExprPeakDepth(*ret.Result(), visited) : 0;
    }
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
    if (kind == NK_AssignStmt) {
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
    if (kind == NK_IfStmt) {
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
    if (kind == NK_WhileStmt) {
        auto& whileStmt = static_cast<SnWhileStmt&>(stmt);
        //Claim 1 stages the condition (codegen, round-9).
        uint16_t d = 1 + ExprPeakDepth(*whileStmt.Cond(), visited);
        if (whileStmt.Body()) {
            uint16_t bd = StmtPeakDepth(*whileStmt.Body(), visited);
            if (bd > d) d = bd;
        }
        return d;
    }
    if (kind == NK_DoStmt) {
        auto& dw = static_cast<SnDoStmt&>(stmt);
        //Claim 1 stages the condition (codegen, round-9).
        uint16_t d = 1 + ExprPeakDepth(*dw.Cond(), visited);
        if (dw.Body()) {
            uint16_t bd = StmtPeakDepth(*dw.Body(), visited);
            if (bd > d) d = bd;
        }
        return d;
    }
    if (kind == NK_ForStmt) {
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
    if (kind == NK_SwitchStmt) {
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
    if (kind == NK_ForeachStmt) {
        auto& fe = static_cast<SnForeachStmt&>(stmt);
        uint16_t d = ExprPeakDepth(*fe.Iterable(), visited);
        if (fe.Body()) {
            uint16_t bd = StmtPeakDepth(*fe.Body(), visited);
            if (bd > d) d = bd;
        }
        return d;
    }
    if (kind == NK_BreakStmt || kind == NK_ContinueStmt) {
        return 0;
    }
    if (kind == NK_AssertStmt) {
        auto& as = static_cast<SnAssertStmt&>(stmt);
        //Claim 1 stages the condition (codegen, round-9).
        return 1 + ExprPeakDepth(*as.Cond(), visited);
    }
    if (kind == NK_TryStmt) {
        auto& ts = static_cast<SnTryStmt&>(stmt);
        uint16_t d = 0;
        if (ts.TryBody()) { uint16_t bd = StmtPeakDepth(*ts.TryBody(), visited); if (bd > d) d = bd; }
        for (auto* c : ts.Catches()) {
            if (c->Body()) { uint16_t bd = StmtPeakDepth(*c->Body(), visited); if (bd > d) d = bd; }
        }
        if (ts.FinallyBody()) { uint16_t fd = StmtPeakDepth(*ts.FinallyBody(), visited); if (fd > d) d = fd; }
        return d;
    }
    if (kind == NK_SuperCallStmt) {
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
    if (kind == NK_ThrowStmt) {
        auto& th = static_cast<SnThrowStmt&>(stmt);
        //Claim 1 stages the thrown expression (codegen, round-9).
        return th.Expr() ? 1 + ExprPeakDepth(*th.Expr(), visited) : 0;
    }
    if (kind == NK_CompoundAssignStmt) {
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
    if (kind == NK_SubscriptAssignStmt) {
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
    return 0;
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
    struct MaxArgsWalker {
        uint16_t maxArgs = 1;
        void walkExpr(SnExpression& expr, bool isMethodContext = false) {
            if (expr.Kind() == NK_InvokeExpr) {
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
                    walkExpr(param);
                return;
            }
            //Recurse into children for nested calls.
            if (expr.Kind() == NK_BinaryExpr) {
                auto& bin = static_cast<SnBinaryExpr&>(expr);
                walkExpr(*bin.Left());
                if (bin.Right()) walkExpr(*bin.Right());
            } else if (expr.Kind() == NK_CastExpr) {
                walkExpr(*static_cast<SnCastExpr&>(expr).Source());
            } else if (expr.Kind() == NK_AsExpr) {
                walkExpr(*static_cast<SnAsExpr&>(expr).Operand());
            } else if (expr.Kind() == NK_NamedArgExpr) {
                walkExpr(*static_cast<SnNamedArgExpr&>(expr).Inner());
            } else if (expr.Kind() == NK_OutArgExpr) {
                //Phase 9e: out arg — walk the inner identifier.
                walkExpr(*static_cast<SnOutArgExpr&>(expr).Inner());
            } else if (expr.Kind() == NK_SubscriptExpr) {
                auto& sub = static_cast<SnSubscriptExpr&>(expr);
                //List/Dict subscript lowers to a synthetic get() call that
                //bulk-copies 2 slots (this + index) into callParamBase —
                //mirror the codegen so maxArgs never under-reserves the
                //region (a lone `li[0]` with no other calls would size
                //callParamBase at 1 and the get() would overflow it).
                if (IsContainerSubscript(*sub.Array()) && maxArgs < 2)
                    maxArgs = 2;
                walkExpr(*sub.Array());
                walkExpr(*sub.Index());
            } else if (expr.Kind() == NK_MemberExpr) {
                auto& member = static_cast<SnMemberExpr&>(expr);
                walkExpr(*member.Outer(), false);
                //If Inner is InvokeExpr, pass method-context flag so
                //slot 0 is reserved for `this` — EXCEPT delegate invokes
                //(a Func-typed field): their args are staged without this
                //and the invoke branch adds the callee scratch instead
                //(mirror ExprPeakDepth's MemberExpr branch).
                if (member.Inner()
                    && member.Inner()->Kind() == NK_InvokeExpr) {
                    auto& innerInvoke =
                        static_cast<SnInvokeExpr&>(*member.Inner());
                    walkExpr(*member.Inner(),
                        !IsDelegateInvoke(innerInvoke));
                } else {
                    walkExpr(*member.Inner(), false);
                }
            } else if (expr.Kind() == NK_NewExpr) {
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
                    walkExpr(arg);
                }
            } else if (expr.Kind() == NK_NewArrayExpr) {
                walkExpr(*static_cast<SnNewArrayExpr&>(expr).Size());
            } else if (expr.Kind() == NK_InitListExpr) {
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
                    if (entry.pValue) walkExpr(*entry.pValue);
            }
        }
        void walkStmt(SnStatement& stmt) {
            NodeKind kind = stmt.Kind();
            if (kind == NK_Paragraph) {
                for (auto& child : static_cast<SnParagraph&>(stmt).Statements())
                    walkStmt(child);
            } else if (kind == NK_ReturnStmt) {
                auto* r = static_cast<SnReturnStmt&>(stmt).Result();
                if (r) walkExpr(*r);
            } else if (kind == NK_InvokeStmt) {
                walkExpr(*static_cast<SnInvokeStmt&>(stmt).Expr());
            } else if (kind == NK_AssignStmt) {
                //Walk the Left() lvalue too — `li[i].f = v` receivers emit
                //a synthetic get() call needing 2 callParamBase slots.
                auto& as = static_cast<SnAssignStmt&>(stmt);
                auto* l = as.Left();
                if (l) walkExpr(*l);
                auto* v = as.Right();
                if (v) walkExpr(*v);
            } else if (kind == NK_IfStmt) {
                auto& ifStmt = static_cast<SnIfStmt&>(stmt);
                walkExpr(*ifStmt.Cond());
                if (ifStmt.ThenStmt()) walkStmt(*ifStmt.ThenStmt());
                if (ifStmt.ElseStmt()) walkStmt(*ifStmt.ElseStmt());
            } else if (kind == NK_WhileStmt) {
                auto& w = static_cast<SnWhileStmt&>(stmt);
                walkExpr(*w.Cond());
                if (w.Body()) walkStmt(*w.Body());
            } else if (kind == NK_DoStmt) {
                auto& dw = static_cast<SnDoStmt&>(stmt);
                walkExpr(*dw.Cond());
                if (dw.Body()) walkStmt(*dw.Body());
            } else if (kind == NK_ForStmt) {
                auto& f = static_cast<SnForStmt&>(stmt);
                if (f.Init()) walkStmt(*f.Init());
                //Round-10: decomposed init assigns (local-decl split) may
                //contain calls — mirror the codegen's InitExtras loop.
                for (auto* pExtra : f.InitExtras()) walkStmt(*pExtra);
                if (f.Cond()) walkExpr(*f.Cond());
                if (f.Fini()) walkStmt(*f.Fini());
                if (f.Body()) walkStmt(*f.Body());
            } else if (kind == NK_SwitchStmt) {
                auto& sw = static_cast<SnSwitchStmt&>(stmt);
                walkExpr(*sw.Cond());
                for (auto* c : sw.Cases()) {
                    //SnCaseClause inherits SyntaxNode, not SnStatement.
                    for (auto* pLabel : c->Labels())
                        walkExpr(*pLabel);
                    if (c->Body()) {
                        for (auto& s : c->Body()->Statements())
                            walkStmt(s);
                    }
                }
                //Round-10: default clause body — emitted after the cases.
                if (sw.Default()) walkStmt(*sw.Default());
            } else if (kind == NK_ForeachStmt) {
                auto& fe = static_cast<SnForeachStmt&>(stmt);
                //Round-11: the List/Dict expansion body-prelude lowers to a
                //synthetic get(i) call that bulk-writes {this, index} into
                //callParamBase — reserve them (SubscriptExpr precedent).
                if (maxArgs < 2) maxArgs = 2;
                walkExpr(*fe.Iterable());
                if (fe.Body()) walkStmt(*fe.Body());
            } else if (kind == NK_AssertStmt) {
                walkExpr(*static_cast<SnAssertStmt&>(stmt).Cond());
            } else if (kind == NK_TryStmt) {
                auto& ts = static_cast<SnTryStmt&>(stmt);
                if (ts.TryBody()) walkStmt(*ts.TryBody());
                for (auto* c : ts.Catches()) {
                    if (c->Body()) walkStmt(*c->Body());
                }
                if (ts.FinallyBody()) walkStmt(*ts.FinallyBody());
            } else if (kind == NK_SuperCallStmt) {
                //super(args) claims 1 (this) + argc slots at call time.
                auto& sc = static_cast<SnSuperCallStmt&>(stmt);
                uint16_t claimSize = static_cast<uint16_t>(1 + sc.Args().size());
                if (claimSize > maxArgs) maxArgs = claimSize;
                for (auto* arg : sc.Args())
                    walkExpr(*arg);
            } else if (kind == NK_ThrowStmt) {
                auto* e = static_cast<SnThrowStmt&>(stmt).Expr();
                if (e) walkExpr(*e);
            } else if (kind == NK_CompoundAssignStmt) {
                auto& ca = static_cast<SnCompoundAssignStmt&>(stmt);
                //Walk Left() receiver too — `mk().x += 1` hides a call
                //needing callParamBase slots (C1-family asymmetry).
                if (ca.Left()) walkExpr(*ca.Left());
                if (ca.Right()) walkExpr(*ca.Right());
            } else if (kind == NK_SubscriptAssignStmt) {
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
                walkExpr(*sa.Array());
                walkExpr(*sa.Index());
                walkExpr(*sa.Value());
            }
        }
    };

    MaxArgsWalker walker;
    if (sn.Body()) {
        for (auto& stmt : sn.Body()->Statements())
            walker.walkStmt(stmt);
    }
    stats.maxArgs = walker.maxArgs;
    return stats;
}

} //namespace nlang
