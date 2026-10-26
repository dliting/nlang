/*---
    StatementResolverSwitchTry.cpp — switch/try 语句解析：标签族映射、
    重复标签检测与异常语句。super(...) 解析在 StatementResolverSuper.cpp。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"
#include "ExprResolverCastFit.hpp"
#include <nlang/runtime/PrimitiveTypes.h>
#include <vector>

namespace nlang
{

//Kind→family mapper for the switch label model — file-static: this TU's
//duplicate-detection and family-gating arms are its only consumers.
//0.7.5: registry-driven — every integer-category row (byte..ulong)
//joins the Int family (one 64-bit key domain); float rows stay Float;
//char joins Int with the string bridge (labels dedup as int64 code
//points); bool stays None by design (spec section 3.4).
static StatementResolveAccessor::SwitchFamily SwitchFamilyOfKind(NodeKind kind)
{
	if (kind == NK_String)
		return StatementResolveAccessor::SwitchFamily::String;
	int pi = ScalarPrimIndexOf(kind);
	if (pi >= 0)
	{
		const auto c = kScalarPrims[pi].category;
		if (c == PC_SInt || c == PC_UInt)
			return StatementResolveAccessor::SwitchFamily::Int;
		if (c == PC_Float)
			return StatementResolveAccessor::SwitchFamily::Float;
		if (c == PC_Char)
			return StatementResolveAccessor::SwitchFamily::Int;
	}
	if (kind == NK_EnumDecl)
		return StatementResolveAccessor::SwitchFamily::Int;
	return StatementResolveAccessor::SwitchFamily::None;
}

//Enum member reference (`Color.Red`): the resolved Field() chain
//carries the member node.
bool StatementResolveAccessor::ExtractEnumMemberLabel(SnExpression& label,
	SwitchLabelKey& key)
{
	auto* pField = static_cast<SnFieldExpr&>(label).Field();
	if (pField && pField->Kind() == NK_EnumMember)
	{
		auto& member = static_cast<SnEnumMember&>(*pField);
		//Defense in depth. In the normal flow the value pre-pass
		//(Resolve -> PreAssignEnumMemberValues) has already
		//assigned values for every enum by the time any switch
		//resolves, so this gate never fires. It guards against a
		//future reordering (e.g. the pre-pass removed or the data
		//pass's early NF_Resolved trusted again) reintroducing
		//stale-0 member keys as false duplicates.
		auto* pDecl = member.Parent();
		if (!pDecl || pDecl->Kind() != NK_EnumDecl
			|| !pDecl->ContainFlags(NF_Resolved))
			return false;
		key.family = SwitchFamily::Int;
		key.intValue = member.Value();
		return true;
	}
	return false;
}

bool StatementResolveAccessor::ExtractLiteralLabel(SnLiteralExpr& lit,
	SwitchLabelKey& key)
{
	auto* pType = lit.EvalDataType();
	if (!pType)
		return false;
	switch (pType->Kind())
	{
	case NK_Int32:
		key.family = SwitchFamily::Int;
		key.intValue = lit.Value().Get<int32_t>();
		return true;
	case NK_Long:
		key.family = SwitchFamily::Int;
		key.intValue = lit.Value().Get<int64_t>();
		return true;
	case NK_ULong:
		key.family = SwitchFamily::Int;
		key.intValue = static_cast<int64_t>(
			lit.Value().Get<uint64_t>());
		return true;
	case NK_Char:
		//0.7.5 string bridge: char labels join the Int key domain —
		//the carrier's raw 4 bytes ARE the code point (bit-preserving
		//Get, same as EmitScalarLiteral's char arm).
		key.family = SwitchFamily::Int;
		key.intValue = lit.Value().Get<int32_t>();
		return true;
	case NK_Float:
		key.family = SwitchFamily::Float;
		key.floatValue = lit.Value().Get<float>();
		return true;
	case NK_Double:
		key.family = SwitchFamily::Float;
		key.floatValue = lit.Value().Get<double>();
		return true;
	case NK_String:
	{
		key.family = SwitchFamily::String;
		auto* pStr = lit.Value().Data().m_String;
		key.stringValue = pStr ? *pStr : "";
		return true;
	}
	default:
		return false;
	}
}

bool StatementResolveAccessor::ExtractSwitchLabelKey(SnExpression& label,
	SwitchLabelKey& key)
{
	if (!label.IsResolved())
		return false;
	if (label.Kind() == NK_MemberExpr || label.Kind() == NK_IdentifierExpr)
		return ExtractEnumMemberLabel(label, key);
	if (label.Kind() == NK_LiteralExpr)
		return ExtractLiteralLabel(static_cast<SnLiteralExpr&>(label), key);
	//0.7.5 sign retirement: negative numeric labels parse as OP_Neg
	//over a literal (`case -1:`). The shared constant fold handles the
	//negation (and the 2^63 INT64_MIN shape), so route through it;
	//negated ulong stays non-extractable like before.
	if (label.Kind() == NK_BinaryExpr)
	{
		ConstLiteralValue v;
		if (TryGetConstantLiteral(&label, v))
		{
			if (v.kind == NK_Int32 || v.kind == NK_Long)
			{
				key.family = SwitchFamily::Int;
				key.intValue = v.i;
				return true;
			}
			if (v.kind == NK_Double || v.kind == NK_Float)
			{
				key.family = SwitchFamily::Float;
				key.floatValue = v.d;
				return true;
			}
		}
	}
	return false;
}

//Report one error per redundant occurrence of a foldable value.
//Int-family keys (int literals + enum members, D2 one family) share
//one set; float keys compare as doubles, so 0.0 and -0.0 are one key
//(they match the same discriminant under IEEE equality).
void StatementResolveAccessor::CheckDuplicateCaseLabels(SnSwitchStmt& sn)
{
	std::vector<SwitchLabelKey> seen;
	std::vector<std::string> reprs;
	for (auto* pCase : sn.Cases())
	{
		for (auto* pLabel : pCase->Labels())
		{
			SwitchLabelKey key;
			if (!ExtractSwitchLabelKey(*pLabel, key))
				continue;
			bool duplicate = false;
			for (size_t n = 0; n < seen.size() && !duplicate; ++n)
			{
				if (seen[n].family != key.family)
					continue;
				duplicate = key.family == SwitchFamily::Int
					? seen[n].intValue == key.intValue
					: key.family == SwitchFamily::Float
					? seen[n].floatValue == key.floatValue
					: seen[n].stringValue == key.stringValue;
				if (duplicate)
					m_Env.Log(CLL_Error, pLabel->Location(),
						"duplicate case label '%s'", reprs[n].c_str());
			}
			if (duplicate)
				continue;
			seen.push_back(key);
			if (key.family == SwitchFamily::Int)
				reprs.push_back(std::to_string(key.intValue));
			else if (key.family == SwitchFamily::Float)
			{
				char buf[32];
				snprintf(buf, sizeof(buf), "%g", key.floatValue);
				reprs.push_back(buf);
			}
			else
				reprs.push_back(key.stringValue);
		}
	}
}

//0.7.5: foldable labels must be representable in the discriminant's own
//type. The emission-side normalize (EmitSwitchLabelNormalize) PrimCasts
//the staged label to the compare width, so an out-of-range label
//silently truncates — `case -9223372036854775808` on an int switch
//compared as 0 and matched x == 0 (probed); a double label beyond float
//precision on a float switch truncates the same way. Same rule as the
//assignment-side constant-fit gate, so `switch (f) case 0.1:` rejects
//exactly where `float f = 0.1;` does.
void StatementResolveAccessor::CheckSwitchLabelFitsDiscriminant(
	SnExpression& label, NodeKind condKind)
{
	//Enum discriminants compare as their int32 value (SwitchCompareOf).
	if (condKind == NK_EnumDecl)
		condKind = NK_Int32;
	int ri = ScalarPrimIndexOf(condKind);
	if (ri < 0)
		return;                     //string discriminant: no range domain
	const ScalarPrimInfo& row = kScalarPrims[ri];

	//Enum member labels carry a bare int32 value — the member's own
	//value may still exceed a narrower discriminant (E.A = 300 on a byte
	//switch can never match). Other member/identifier labels are not
	//enum members and have nothing constant to gate.
	if (label.Kind() == NK_MemberExpr || label.Kind() == NK_IdentifierExpr)
	{
		SwitchLabelKey key;
		if (ExtractEnumMemberLabel(label, key)
			&& !IntFitsRow(key.intValue, row))
			m_Env.Log(CLL_Error, label.Location(),
				"case label %lld is out of range for the switch "
				"discriminant '%s'",
				(long long)key.intValue, row.name);
		return;
	}

	//Literal / negated literal: TryConstantFit applies the same domain
	//and range predicates as the assignment-side gate and renders the
	//constant text. Non-constant labels (calls, variables) come back
	//CF_NotApplicable — runtime first-match-wins, nothing to gate.
	auto fit = TryConstantFit(label, row);
	if (fit.verdict == CF_OutOfRange)
		m_Env.Log(CLL_Error, label.Location(),
			"case label %s is out of range for the switch "
			"discriminant '%s'",
			fit.constantText, row.name);
}

void StatementResolveAccessor::Access(SnSwitchStmt &sn)
{
	assert(m_pVisitor);
	sn.Cond()->Accept(*m_pVisitor);
	//D1 family gate. An unresolved cond already reported its own
	//error — skip the family check to avoid cascades. The null
	//literal is Int32-typed (nlang.y KT_Null) and would slip through
	//the family check as Int — reject it on the cond side too,
	//mirroring the label side. Array values need no dedicated arm:
	//an array-valued cond carries its interned array token, whose
	//kind falls into the None family below (0.7.3 B).
	if (sn.Cond()->ContainFlags(NF_NullLiteral))
		m_Env.Log(CLL_Error, sn.Cond()->Location(),
			"switch discriminant must be int, float, string, or enum");
	else if (sn.Cond()->IsResolved())
	{
		auto* pType = sn.Cond()->EvalDataType();
		if (pType
			&& SwitchFamilyOfKind(pType->Kind()) == SwitchFamily::None)
			m_Env.Log(CLL_Error, sn.Cond()->Location(),
				"switch discriminant must be int, float, string, or enum");
	}
	//v0.9.1: case bodies are breakable scope (a bare break may leave the
	//switch) but not loop scope (continue passes through to the enclosing
	//loop) — guard only the breakable depth, not the loop depth.
	FlowDepthGuard guard(m_breakableDepth);
	for (auto* pCase : sn.Cases())
		pCase->Accept(*m_pVisitor);
	if (sn.Default())
		sn.Default()->Accept(*m_pVisitor);
	CheckDuplicateCaseLabels(sn);
}

void StatementResolveAccessor::Access(SnCaseClause &sn)
{
	assert(m_pVisitor);
	//D2: every label must belong to the discriminant's family. The
	//cond's resolved type is read back through the parent switch
	//(zero new AST state); an unresolved cond skips the check.
	auto* pParent = sn.Parent();
	auto* pSwitch = (pParent && pParent->Kind() == NK_SwitchStmt)
		? static_cast<SnSwitchStmt*>(pParent) : nullptr;
	auto* pCondType = pSwitch ? pSwitch->Cond()->EvalDataType() : nullptr;
	auto condFamily = pCondType
		? SwitchFamilyOfKind(pCondType->Kind()) : SwitchFamily::None;
	for (auto* pLabel : sn.Labels())
	{
		pLabel->Accept(*m_pVisitor);
		if (!pLabel->IsResolved())
			continue;   //its own resolution already reported
		if (pLabel->ContainFlags(NF_NullLiteral))
		{
			m_Env.Log(CLL_Error, pLabel->Location(),
				"null is not a valid case label");
			continue;
		}
		if (condFamily == SwitchFamily::None)
			continue;
		auto* pLabelType = pLabel->EvalDataType();
		if (pLabelType
			&& SwitchFamilyOfKind(pLabelType->Kind()) != condFamily)
			m_Env.Log(CLL_Error, pLabel->Location(),
				"case label type must match the switch discriminant family");
		else if (condFamily != SwitchFamily::None)
			CheckSwitchLabelFitsDiscriminant(*pLabel,
				pCondType->Kind());
	}
	sn.Body()->Accept(*m_pVisitor);
}

//Phase 9d: try body and catch clauses. Phase 9d-2 adds the optional
//finally body. Either at least one catch or a finally body is required;
//each catch type must be Exception or a subclass; each catch var is
//registered in the body's enclosing paragraph scope. Control-flow
//statements (break/continue/return/throw) are rejected inside a finally
//body (m_inFinallyBody flag).
void StatementResolveAccessor::Access(SnTryStmt &sn)
{
	if (sn.IsResolved())
		return;
	assert(m_pVisitor);
	if (sn.Catches().empty() && !sn.FinallyBody()) {
		m_Env.Log(CLL_Error, sn.Location(),
			"try statement must have at least one catch clause or a finally block");
		sn.AddFlags(NF_Resolved);
		return;
	}
	if (sn.TryBody())
		sn.TryBody()->Accept(*m_pVisitor);
	for (auto* pCatch : sn.Catches())
		pCatch->Accept(*m_pVisitor);
	if (sn.FinallyBody()) {
		bool prev = m_inFinallyBody;
		m_inFinallyBody = true;
		sn.FinallyBody()->Accept(*m_pVisitor);
		m_inFinallyBody = prev;
	}
	sn.AddFlags(NF_Resolved);
}

void StatementResolveAccessor::Access(SnCatchClause &sn)
{
	//1. Resolve catch type (must be Exception or subclass).
	sn.CatchType()->Accept(*m_pVisitor);
	if (!sn.CatchType()->IsResolved()
		|| !IsExceptionSubclass(sn.CatchType()->Field())) {
		m_Env.Log(CLL_Error, sn.Location(),
			"catch type must be Exception or a subclass");
	}

	//2. Register the catch var in the catch BODY's own paragraph so the
	//var is scoped to its clause. Every clause of a function shares the
	//enclosing paragraph and AddLocal is first-wins, so enclosing-
	//paragraph registration made a second same-named var silently drop
	//and its references bind to the first clause's type (a user
	//subclass's own field then failed to resolve — or worse, a smaller
	//builtin layout was read out of bounds at runtime). A bare-statement
	//body (no braces) has no paragraph of its own; the var keeps the
	//legacy enclosing-paragraph registration there.
	auto *pEnclosing = FindEnclosingParagraph(sn.Parent());
	auto *pBodyParagraph = (sn.Body()
		&& sn.Body()->Kind() == NK_Paragraph)
		? static_cast<SnParagraph*>(sn.Body()) : nullptr;
	auto *pScope = pBodyParagraph ? pBodyParagraph : pEnclosing;
	//3. Register catch var (typed by catch type; assignable in body).
	if (pScope && sn.CatchType()->IsResolved()
		&& sn.CatchType()->Field()) {
		//0.9.0 semantic restriction: the catch parameter may not reuse
		//a visible type name. The gate still consults the ENCLOSING
		//paragraph — the scope the clause shares with sibling code.
		if (pEnclosing)
			RejectTypeNameVariable(sn.VarName(), *pEnclosing, *m_pCurrType,
				sn.Location());
		auto *pLocal = new SnLocalVar(sn.VarName(),
			sn.CatchType()->Field(), *sn.Location());
		pScope->AddLocal(sn.VarName(), pLocal);
	}

	//4. Resolve body.
	if (sn.Body())
		sn.Body()->Accept(*m_pVisitor);
}

void StatementResolveAccessor::Access(SnThrowStmt &sn)
{
	if (sn.IsResolved())
		return;
	assert(m_pVisitor);
	if (m_inFinallyBody) {
		m_Env.Log(CLL_Error, sn.Location(),
			"throw is not allowed inside a finally block");
		sn.AddFlags(NF_Resolved);
		return;
	}
	if (sn.IsRethrow()) {
		//throw; — must be lexically inside a catch handler. Walk
		//parent chain looking for NK_CatchClause.
		auto pParent = sn.Parent();
		bool inCatch = false;
		while (pParent) {
			if (pParent->Kind() == NK_CatchClause) {
				inCatch = true;
				break;
			}
			pParent = pParent->Parent();
		}
		if (!inCatch)
			m_Env.Log(CLL_Error, sn.Location(),
				"throw; (rethrow) is only valid inside a catch block");
	} else {
		sn.Expr()->Accept(*m_pVisitor);
		//EvalDataType is populated by the resolver after Accept().
		auto pType = sn.Expr()->EvalDataType();
		if (pType && !IsExceptionSubclass(pType)) {
			m_Env.Log(CLL_Error, sn.Location(),
				"throw expression must be Exception or a subclass");
		}
	}
	sn.AddFlags(NF_Resolved);
}

} //namespace nlang
