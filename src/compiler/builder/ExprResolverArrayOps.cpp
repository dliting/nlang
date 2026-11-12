/*---
    ExprResolverArrayOps.cpp — v0.14.0 numpy-style array arithmetic.
    `a + b`, `2 * x`, `1 - a`, `-a` on numeric-element arrays resolve
    AS tensor calls: the binary node keeps its position in the tree
    (every holder representation — child list, typed slots, active
    traversal frames — stays untouched), and resolution binds onto it
    the selected tensor-callee overload plus an emission-plan invoke
    whose params are the node's own operands. Codegen emits the plan
    through the standard invoke path (staging, safepoints and the
    native descriptor all reused), so the emitted bytecode is
    indistinguishable from a hand-written tensor.add(a, b).

    Design appendix: docs/superpowers/specs/2026-11-12-m5-array-
    operator-lowering-design.md (three adversarial review rounds; the
    emitter-side lowering replaced mid-traversal node surgery after
    review rounds surfaced its UAF/holder-coverage costs).

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
//The element row of an array-typed field (either spelling), or null.
SnField *ArrayElementRowOf(const SnField *pType)
{
	if (!pType)
		return nullptr;
	if (pType->Kind() == NK_ArrayTypeToken)
		return static_cast<SnArrayTypeToken *>(
			const_cast<SnField *>(pType))->ElemTypeOf();
	return ArrayInstantiationElem(const_cast<SnField *>(pType));
}

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

} //namespace

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

	const char *opName = OpSpelling(op);

	//Unary minus on an array: -a -> tensor.neg(a). The grammar gives
	//unary negation its own operator (OP_Neg, no right operand).
	const bool unary = !sn.Right();
	if (unary && op != SnBinaryExpr::OP_Neg)
		return false;

	//Invalid-operand propagation: a nested operand already diagnosed
	//(NF_Invalid) consumes this node silently — one diagnostic per
	//source error, no cascade along the expression chain.
	if (sn.Left()->IsInvalid()
		|| (sn.Right() && sn.Right()->IsInvalid()))
	{
		sn.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
		sn.AddFlags(NF_Resolved | NF_Invalid);
		return true;
	}

	auto *pLElem = ArrayElementRowOf(pL);
	auto *pRElem = ArrayElementRowOf(pR);
	const SnField *pArrElem = lArr ? pLElem : pRElem;
	if ((lArr && !IsNumericElementRow(pLElem))
		|| (rArr && !IsNumericElementRow(pRElem)))
	{
		if (IsByteElementRow(pArrElem))
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"operator '%s': array element kind \"byte\" has no "
				"arithmetic.", opName);
		}
		else
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"operator '%s': array element kind \"%s\" has no "
				"arithmetic.",
				opName, pArrElem ? pArrElem->ToString().c_str() : "?");
		}
		sn.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
		sn.AddFlags(NF_Resolved | NF_Invalid);
		return true;
	}

	//Array x array: the element rows must be identical (no silent
	//widening between arrays — the language-wide rule).
	if (lArr && rArr && pLElem != pRElem)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '%s': element kind mismatch (\"%s\" vs \"%s\").",
			opName, pL->ToString().c_str(), pR->ToString().c_str());
		sn.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
		sn.AddFlags(NF_Resolved | NF_Invalid);
		return true;
	}

	//Import admission at classification time: one diagnostic per
	//operator use even in nested expressions (the lowered call would
	//fail unit discovery — C1).
	auto &reg = m_Env.Registry();
	const uint32_t curModule = reg.OwnerOfContext(sn);
	if (!reg.ImportGateAllows(curModule, "tensor"))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Package 'tensor' is not imported. Add 'import tensor;' "
			"at the top of this file (array arithmetic lowers to the "
			"tensor package).");
		sn.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
		sn.AddFlags(NF_Resolved | NF_Invalid);
		return true;
	}

	//Bind the callee + emission plan. The plan invoke is a detached
	//emission node: its params are this node's own operands (ownership
	//moves from the binary's child list to the plan — the binary stays
	//in the tree, childless but with its operand slots pointing at the
	//live plan-owned operands). MatchInvokeAgainst selects the element-
	//kind overload and produces the formal bindings the emitter stages.
	const bool scalarLeft = !lArr && rArr;
	const char *fnName = TensorFnFor(op, unary ? false : scalarLeft);
	auto candidates = reg.ModuleFunctions("tensor", fnName);
	if (candidates.empty())
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '%s': the tensor package exposes no \"%s\" "
			"primitive.", opName, fnName);
		sn.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
		sn.AddFlags(NF_Resolved | NF_Invalid);
		return true;
	}

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
		delete pPlan;
		sn.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
		sn.AddFlags(NF_Resolved | NF_Invalid);
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
		delete pPlan;
		sn.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
		sn.AddFlags(NF_Resolved | NF_Invalid);
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

} //namespace nlang
