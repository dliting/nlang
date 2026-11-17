/*---
    ExprResolverArrayOps.cpp — v0.13.7 numpy-style array arithmetic.
    `a + b`, `2 * x`, `1 - a`, `-a` on numeric-element arrays resolve
    AS tensor calls: the binary node keeps its position in the tree
    (every holder representation — child list, typed slots, active
    traversal frames — stays untouched), and resolution binds onto it
    the selected tensor-callee overload plus an emission-plan invoke
    whose params are the node's own operands. Codegen emits the plan
    through the standard invoke path (staging, safepoints and the
    native descriptor all reused), so the emitted bytecode is
    indistinguishable from a hand-written tensor.add(a, b).

    Design note: the emitter-side lowering replaced mid-traversal node
    surgery — the earlier plan swapped nodes while the resolver walked
    the tree, whose ownership and holder-coverage costs adversarial
    review surfaced (three rounds). The failure paths unwind through
    UnwindTensorCallPlan so a failed match/resolve leaves the operands
    owned by the binary, exactly the shape it never would have left.

    The call requires `import tensor` like every other tensor.* call:
    the import drives library-unit discovery (an unimported unit is
    not inlined, so there is nothing to call). Admission is checked at
    classification time — one diagnostic per operator use even in
    nested expressions. Compound assignment on arrays is rejected by
    name in StatementResolverCompound.
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <string>

namespace nlang
{

namespace
{
//ArrayElementRowOf — the shared element-row derivation (both spellings)
//lives in SnArrayTypeToken.cpp next to ArrayInstantiationElem.

bool IsArrayRow(const SnField *pType)
{
	return pType && (pType->Kind() == NK_ArrayTypeToken
		|| ArrayInstantiationElem(const_cast<SnField *>(pType)) != nullptr);
}

bool IsNumericElementRow(const SnField *pElem)
{
	if (!pElem)
		return false;
	const NodeKind kind = pElem->Kind();
	return kind == NK_Int32 || kind == NK_Long
		|| kind == NK_Float || kind == NK_Double;
}

bool IsByteElementRow(const SnField *pElem)
{
	return pElem && (pElem->Kind() == NK_Byte
		|| pElem->Kind() == NK_UByte);
}

//A nested operand already diagnosed (NF_Invalid): the chain consumes
//the node silently so one source error yields one diagnostic.
bool NestedOperandInvalid(const SnBinaryExpr &sn)
{
	return sn.Left()->IsInvalid()
		|| (sn.Right() && sn.Right()->IsInvalid());
}

//The comparison family this lowering serves (mirrors the static
//helper in ExprResolverBinary.cpp; the resolver TUs keep their own).
static bool IsCompareOp(SnBinaryExpr::Operator op)
{
	return op == SnBinaryExpr::OP_Equal
		|| op == SnBinaryExpr::OP_NotEqual
		|| op == SnBinaryExpr::OP_Greater
		|| op == SnBinaryExpr::OP_GreaterEqual
		|| op == SnBinaryExpr::OP_Less
		|| op == SnBinaryExpr::OP_LessEqual;
}

const char *OpSpelling(SnBinaryExpr::Operator op)
{
	switch (op)
	{
		case SnBinaryExpr::OP_Add: return "+";
		case SnBinaryExpr::OP_Sub: return "-";
		case SnBinaryExpr::OP_Mul: return "*";
		case SnBinaryExpr::OP_Div: return "/";
		case SnBinaryExpr::OP_Neg: return "-";
		default: return "?";
	}
}

//The tensor primitive for the shape: array-array keeps the plain
//family; array-scalar the scalar arms; scalar-array swaps into the
//reversed arms for `-`/`/` (no commute) and the ordinary arms for
//`+`/`*` (commutative — the operand order swaps at the call).
const char *TensorFnFor(SnBinaryExpr::Operator op, bool scalarLeft)
{
	if (op == SnBinaryExpr::OP_Neg)
		return "neg";
	//Comparisons: the base spelling carries a scalar overload at the
	//same name (the add/mul precedent — overload resolution picks the
	//(Array, scalar) arm when the right operand is a scalar), so only a
	//scalar LEFT needs a reflected spelling: 5 < a ≡ a > 5 binds
	//gtScalar (the spelling computes a[i] > v with the operands
	//unchanged), the relational precedent of rsub/rdiv.
	if (IsCompareOp(op))
	{
		switch (op)
		{
			case SnBinaryExpr::OP_Equal:
			case SnBinaryExpr::OP_NotEqual:
				return scalarLeft ? (op == SnBinaryExpr::OP_Equal
					? "eqScalar" : "neScalar")
					: (op == SnBinaryExpr::OP_Equal ? "eq" : "ne");
			case SnBinaryExpr::OP_Greater:
				return scalarLeft ? "ltScalar" : "gt";
			case SnBinaryExpr::OP_GreaterEqual:
				return scalarLeft ? "leScalar" : "ge";
			case SnBinaryExpr::OP_Less:
				return scalarLeft ? "gtScalar" : "lt";
			case SnBinaryExpr::OP_LessEqual:
				return scalarLeft ? "geScalar" : "le";
			default:
				return nullptr;
		}
	}
	switch (op)
	{
		case SnBinaryExpr::OP_Add:
			return scalarLeft ? "addScalar" : "add";
		case SnBinaryExpr::OP_Sub:
			return scalarLeft ? "rsubScalar" : "sub";
		case SnBinaryExpr::OP_Mul:
			return scalarLeft ? "mulScalar" : "mul";
		case SnBinaryExpr::OP_Div:
			return scalarLeft ? "rdivScalar" : "div";
		default:
			return nullptr;
	}
}

//Match/resolve failure unwind: the plan adopted the binary's operands
//(ownership moved at plan construction), so deleting the plan alone
//would delete the operand nodes the binary's slots still point at.
//Detach the params from the plan (erase releases without destroying),
//then let the binary re-attach its slots in the original left-then-
//right child order.
static void UnwindTensorCallPlan(SnBinaryExpr &sn, SnInvokeExpr &plan,
	bool unary)
{
	auto &params = plan.Params();
	//The params are exactly the binary's operand slots (asserted: a
	//replaced param here would mean the slots already dangle and no
	//unwind can repair them).
	NLANG_ASSERT(plan.Children().size() == (unary ? 1 : 2));
	for (auto &param : params)
		NLANG_ASSERT(&param == sn.Left() || &param == sn.Right());
	while (!plan.Children().empty())
		params.erase(params.begin());
	sn.ReattachOperands();
}

} //namespace

//Access's hook for the lowering: this operator set carries array
//operands, and the defensive pre-gates keep the paths whose shared
//hooks own them away from the lowering (a string side belongs to the
//concat path, a null literal to the null arithmetic guard).
bool ExprResolveAccessor::TryTensorBinaryAdmission(SnBinaryExpr &sn)
{
	const auto op = sn.Op();
	if (op != SnBinaryExpr::OP_Add && op != SnBinaryExpr::OP_Sub
		&& op != SnBinaryExpr::OP_Neg && op != SnBinaryExpr::OP_Mul
		&& op != SnBinaryExpr::OP_Div
		&& !IsCompareOp(op))
		return false;
	auto *pLchk = sn.Left()->EvalDataType();
	auto *pRchk = sn.Right() ? sn.Right()->EvalDataType() : nullptr;
	const bool stringSide =
		(pLchk && pLchk->Kind() == NK_String)
		|| (pRchk && pRchk->Kind() == NK_String);
	const bool nullSide = sn.Left()->ContainFlags(NF_NullLiteral)
		|| (sn.Right() && sn.Right()->ContainFlags(NF_NullLiteral));
	if (stringSide || nullSide)
		return false;
	//The comparison family lowers only for NUMERIC element rows — a
	//non-numeric array's ==/!= keep the identity comparison (the
	//compare-gate fallback below). The arithmetic family's numeric
	//gate rides the shape/binding as before.
	if (IsCompareOp(op))
	{
		auto *pLrow = IsArrayRow(pLchk)
			? ArrayElementRowOf(pLchk) : nullptr;
		auto *pRrow = pRchk && IsArrayRow(pRchk)
			? ArrayElementRowOf(pRchk) : nullptr;
		const bool numeric = (pLrow && IsNumericElementRow(pLrow))
			|| (pRrow && IsNumericElementRow(pRrow));
		if (!numeric)
			return false;
	}
	return TryTensorBinaryShape(sn, op);
}

/*
Resolution-time classification (phase 1). True = the node is consumed:
either a lowering-shape binary (flagged NF_TensorOp, bound to its
tensor callee and emission plan — the emitter lowers it in place) or a
lowering-specific diagnostic was logged. False = not an array shape;
the scalar arithmetic path keeps the node.
*/
bool ExprResolveAccessor::TryTensorBinaryShape(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op)
{
	auto *pL = sn.Left()->EvalDataType();
	auto *pR = sn.Right() ? sn.Right()->EvalDataType() : nullptr;
	const bool lArr = IsArrayRow(pL);
	const bool rArr = IsArrayRow(pR);
	if (!lArr && !rArr)
		return false;

	//Unary minus on an array: -a -> tensor.neg(a). The grammar gives
	//unary negation its own operator (OP_Neg, no right operand).
	const bool unary = !sn.Right();
	if (unary && op != SnBinaryExpr::OP_Neg)
		return false;

	//Invalid-operand propagation: one diagnostic per source error, no
	//cascade along the expression chain.
	if (NestedOperandInvalid(sn))
	{
		ConsumeInvalidTensorShape(sn);
		return true;
	}

	const char *opName = OpSpelling(op);
	auto *pLElem = ArrayElementRowOf(pL);
	auto *pRElem = ArrayElementRowOf(pR);
	if ((lArr && !IsNumericElementRow(pLElem))
		|| (rArr && !IsNumericElementRow(pRElem)))
	{
		LogNoArrayArithmetic(sn, opName, lArr ? pLElem : pRElem);
		ConsumeInvalidTensorShape(sn);
		return true;
	}

	//Array x array: the element rows must be identical.
	if (lArr && rArr && pLElem != pRElem)
	{
		LogTensorElemKindMismatch(sn, opName, pL, pR);
		ConsumeInvalidTensorShape(sn);
		return true;
	}

	if (!TensorImportAdmitted(sn, opName))
	{
		ConsumeInvalidTensorShape(sn);
		return true;
	}
	return BindTensorCall(sn, op, unary, lArr, rArr);
}

//Shared epilogue for consumed-with-diagnostic shapes: the node leaves
//resolution as a diagnosed terminal non-value, so no arm of the
//expression chain re-diagnoses it.
void ExprResolveAccessor::ConsumeInvalidTensorShape(SnBinaryExpr &sn)
{
	sn.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
	sn.AddFlags(NF_Resolved | NF_Invalid);
}

//"byte has no arithmetic" is spelled out (its row name reads as a
//generic placeholder otherwise); every other non-numeric kind is
//reported by its row.
void ExprResolveAccessor::LogNoArrayArithmetic(SnBinaryExpr &sn,
	const char *opName, const SnField *pArrElem)
{
	if (IsByteElementRow(pArrElem))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '%s': array element kind \"byte\" has no "
			"arithmetic.", opName);
		return;
	}
	m_Env.Log(CLL_Error, sn.Location(),
		"operator '%s': array element kind \"%s\" has no "
		"arithmetic.",
		opName, pArrElem ? pArrElem->ToString().c_str() : "?");
}

//Array x array with differing element rows — no silent widening
//between arrays (the language-wide rule).
void ExprResolveAccessor::LogTensorElemKindMismatch(SnBinaryExpr &sn,
	const char *opName, const SnField *pL, const SnField *pR)
{
	m_Env.Log(CLL_Error, sn.Location(),
		"operator '%s': element kind mismatch (\"%s\" vs \"%s\").",
		opName, pL->ToString().c_str(), pR->ToString().c_str());
}

//Import admission at classification time: one diagnostic per operator
//use even in nested expressions (the lowered call would fail unit
//discovery — C1).
bool ExprResolveAccessor::TensorImportAdmitted(SnBinaryExpr &sn,
	const char *opName)
{
	auto &reg = m_Env.Registry();
	const uint32_t curModule = reg.OwnerOfContext(sn);
	if (reg.ImportGateAllows(curModule, "tensor"))
		return true;
	m_Env.Log(CLL_Error, sn.Location(),
		"Package 'tensor' is not imported. Add 'import tensor;' "
		"at the top of this file (array arithmetic lowers to the "
		"tensor package).");
	return false;
}

/*
Detached emission plan over the binary's own operands. The plan invoke
is a detached node: its params are this node's operands (ownership
moves from the binary's child list to the plan — the binary stays in
the tree, childless but with its operand slots pointing at the live
plan-owned operands). Returns null when param resolution logs its own
diagnostic; the caller consumes the binary.
*/
SnInvokeExpr *ExprResolveAccessor::BuildTensorCallPlan(SnBinaryExpr &sn,
	const char *fnName, bool unary, bool scalarLeft)
{
	UniquePtrList<SnExpression> params;
	if (unary)
	{
		params.Add(sn.Left());
	}
	else if (scalarLeft)
	{
		params.Add(sn.Right());
		params.Add(sn.Left());
	}
	else
	{
		params.Add(sn.Left());
		params.Add(sn.Right());
	}
	//Detach from the binary's child list, then adopt into the plan
	//(ownership moves — RemoveChildFrom releases, the ctor's list
	//takes it).
	{
		auto &children = sn.Children();
		while (!children.empty())
		{
			auto iChild = children.begin();
			RemoveChildFrom(iChild, sn);
		}
	}

	auto pPlan = new SnInvokeExpr(new std::string(fnName),
		std::move(params), *sn.Location());
	if (!ResolveExpressionList(pPlan->Params()))
	{
		UnwindTensorCallPlan(sn, *pPlan, unary);
		delete pPlan;
		return nullptr;
	}
	return pPlan;
}

/*
Callee/plan binding for a classified lowering shape. MatchInvokeAgainst
selects the element-kind overload and produces the formal bindings the
emitter stages; the plan invoke is stored on the binary (TensorCallPlan)
for EmitExprBinary to lower in place.
*/
bool ExprResolveAccessor::BindTensorCall(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op, bool unary, bool lArr, bool rArr)
{
	const bool scalarLeft = !lArr && rArr;
	const char *fnName = TensorFnFor(op, unary ? false : scalarLeft);
	const char *opName = OpSpelling(op);
	auto candidates = m_Env.Registry().ModuleFunctions("tensor", fnName);
	if (candidates.empty())
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '%s': the tensor package exposes no \"%s\" "
			"primitive.", opName, fnName);
		ConsumeInvalidTensorShape(sn);
		return true;
	}

	auto pPlan = BuildTensorCallPlan(sn, fnName, unary, scalarLeft);
	if (!pPlan)
	{
		ConsumeInvalidTensorShape(sn);
		return true;
	}

	SnFunction *pCallee = nullptr;
	std::vector<FormalBinding> bindings;
	bool bAmbiguous = false;
	auto match = MatchInvokeAgainst(*pPlan, candidates, pCallee,
		bindings, bAmbiguous);
	if (match != FFR_ExactMatch && match != FFR_ApproximateMatch)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '%s': no matching tensor primitive for the "
			"operand kinds.", opName);
		UnwindTensorCallPlan(sn, *pPlan, unary);
		delete pPlan;
		ConsumeInvalidTensorShape(sn);
		return true;
	}
	if (match == FFR_ApproximateMatch)
		FixupParamTypesWithBindings(*pPlan, bindings);
	pPlan->SetBindings(std::move(bindings));

	sn.TensorCallee(pCallee);
	sn.TensorCallPlan(std::unique_ptr<SnInvokeExpr>(pPlan));
	//Result type = the callee's return row (the tensor element array).
	sn.EvalDataType(pCallee->ReturnType()->EvalDataType());
	sn.AddFlags(NF_TensorOp | NF_Resolved);
	return true;
}

//A tensor-flagged binary lowers at codegen via its bound callee — a
//flagged node WITHOUT the binding is the failure shape (classification
//flagged it but the binding failed).
static bool TreeHasUnloweredTensorOp(SyntaxNode &node)
{
	if (node.Kind() == NK_BinaryExpr && node.ContainFlags(NF_TensorOp))
	{
		auto &bin = static_cast<SnBinaryExpr &>(node);
		if (!bin.TensorCallee() || !bin.TensorCallPlan())
			return true;
	}
	for (auto &child : node.Children())
	{
		if (TreeHasUnloweredTensorOp(static_cast<SyntaxNode &>(child)))
			return true;
	}
	return false;
}

bool TensorOpResolutionFailed(BuildEnvironment &env, SyntaxNode &root)
{
	//Partially failed trees pass: residual flagged nodes there are
	//legal (the resolution aborts before finishing the lowering).
	if (env.HasError())
		return false;
	if (!TreeHasUnloweredTensorOp(root))
		return false;
	//Internal inconsistency: no source position to name — the
	//lowering's own diagnostics carry the positions.
	env.Log(CLL_Error,
		"internal: an array arithmetic node reached codegen unlowered");
	return true;
}

} //namespace nlang
