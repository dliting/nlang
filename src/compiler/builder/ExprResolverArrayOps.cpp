/*---
    ExprResolverArrayOps.cpp — v0.14.0 numpy-style array arithmetic.
    `a + b`, `2 * x`, `1 - a`, `-a` on numeric-element arrays lower to
    the stdlib tensor module's kind-dispatching primitives, so every
    spelling rides the tier-dispatched SIMD kernels behind them.

    Two-phase design (full rationale in the M5 design appendix,
    docs/superpowers/specs/2026-11-12-m5-array-operator-lowering-
    design.md):

    1. Resolution (TryTensorBinaryShape, called from
       Access(SnBinaryExpr)): classify once — diagnostics for byte
       arrays, cross-kind pairs, missing import fire here — flag
       NF_TensorOp, and stamp the array operand's row as the node's
       result type (the operand-wrap pass then sees TCK_Same).
    2. Holder swap (BuildTensorCall + the ReplaceChildNode protocol):
       the call node is synthesized from the detached operands and
       spliced through the parent's ReplaceChildNode — the protocol
       keeps every typed slot in sync (the per-class overrides live in
       SnChildSync.cpp), destroys the drained binary, and leaves every
       iterator into the child list valid (the in-place cell
       assignment). Access(SnBinaryExpr) returns IMMEDIATELY after the
       swap — the binary node is gone and must not be touched.

    The call requires `import tensor` like every other tensor.* call:
    the import drives library-unit discovery (an unimported unit is not
    inlined, so there is nothing to call). Admission is checked at
    classification time, so an operator use without the import yields
    exactly one standard "Package 'tensor' is not imported" diagnostic
    even in nested expressions. Compound assignment on arrays is
    rejected by name in StatementResolverCompound.
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
either a lowering-shape binary (flagged NF_TensorOp with the array
result row — the holder swap happens right here in the caller) or a
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
	if (!sn.Right())
	{
		if (op != SnBinaryExpr::OP_Neg || !lArr)
			return false;
		auto *pElem = ArrayElementRowOf(pL);
		if (!IsNumericElementRow(pElem))
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"operator '-': array element kind \"%s\" has no "
				"arithmetic.", pElem ? pElem->ToString().c_str() : "?");
			sn.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
			sn.AddFlags(NF_Resolved | NF_Invalid);
			return true;
		}
		sn.EvalDataType(pL);
		sn.AddFlags(NF_TensorOp | NF_Resolved);
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
	//operator use even in nested expressions (the synthesized call
	//would fail unit discovery — C1), and no swap leaves an untyped
	//node behind to poison the parent's classification.
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

	//Result type = the array operand's row (the scalar arms return the
	//array too).
	sn.EvalDataType(lArr ? pL : pR);
	sn.AddFlags(NF_TensorOp | NF_Resolved);
	return true;
}

/*
Phase 2: build the tensor.<fn>(...) member call from the flagged
binary. The operands are DETACHED from the binary (ownership moves to
the call — the DetachChild contract); the caller splices the call
through the parent's ReplaceChildNode, which destroys the drained
binary — nothing here deletes it, and the caller must not either.
*/
SnMemberExpr *ExprResolveAccessor::BuildTensorCall(SnBinaryExpr &bin)
{
	const bool lArr = IsArrayRow(bin.Left()->EvalDataType());
	const bool scalarLeft = !lArr && bin.Right() != nullptr;
	const char *fnName = TensorFnFor(bin.Op(), scalarLeft);
	NLANG_ASSERT(fnName);

	SnExpression *pLeftNode = nullptr;
	SnExpression *pRightNode = nullptr;
	{
		auto &children = bin.Children();
		auto iL = children.begin();
		pLeftNode = static_cast<SnExpression *>(&*iL);
		RemoveChildFrom(iL, bin);
		if (bin.Right())
		{
			auto iR = children.begin();
			pRightNode = static_cast<SnExpression *>(&*iR);
			RemoveChildFrom(iR, bin);
		}
	}
	UniquePtrList<SnExpression> params;
	if (scalarLeft)
	{
		params.Add(pRightNode);
		params.Add(pLeftNode);
	}
	else
	{
		params.Add(pLeftNode);
		if (pRightNode)
			params.Add(pRightNode);
	}

	auto pInvoke = new SnInvokeExpr(new std::string(fnName),
		std::move(params), *bin.Location());
	auto pModule = new SnIdentifierExpr(new std::string("tensor"),
		*bin.Location());
	return new SnMemberExpr(pModule, pInvoke, *bin.Location());
}

} //namespace nlang
