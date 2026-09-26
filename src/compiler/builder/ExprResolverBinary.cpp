/*---
    ExprResolverBinary.cpp — 二元/逻辑表达式解析（对称提升、比较门、字符串拼接）
    从 ExprResolver.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
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
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

void ExprResolveAccessor::Access(SnBinaryExpr &sn)
{
	assert(!sn.IsResolved());

	sn.Left()->Accept(*m_pVisitor);
	if (!sn.Left()->IsResolved())
		return;

	if (sn.Right())
	{
		sn.Right()->Accept(*m_pVisitor);
		if (!sn.Right()->IsResolved())
			return;
	}

	auto op = sn.Op();
	const bool isCompare = op == SnBinaryExpr::OP_Less
		|| op == SnBinaryExpr::OP_LessEqual
		|| op == SnBinaryExpr::OP_Greater
		|| op == SnBinaryExpr::OP_GreaterEqual
		|| op == SnBinaryExpr::OP_Equal
		|| op == SnBinaryExpr::OP_NotEqual;
	if (isCompare || op == SnBinaryExpr::OP_LogicalAnd ||
		op == SnBinaryExpr::OP_LogicalOr ||
		op == SnBinaryExpr::OP_LogicalNot)
	{
		if (isCompare)
		{
			//Phase 11 Q4: relational operands get type checks here — the
			//old shortcut set Int32 blindly and codegen picked the opcode
			//variant from the left operand alone.
			auto* L = sn.Left()->EvalDataType();
			auto* R = sn.Right() ? sn.Right()->EvalDataType() : nullptr;
			NodeKind lk = L ? L->Kind() : NK_Int32;
			NodeKind rk = R ? R->Kind() : NK_Int32;
			bool lNull = sn.Left()->ContainFlags(NF_NullLiteral);
			bool rNull = sn.Right()
				&& sn.Right()->ContainFlags(NF_NullLiteral);

			//Mixed string/non-string has no semantics ("a" < 5), in either
			//operand order. Null literals are exempt: KT_Null is Int32-
			//typed, and `s == null` / `c == null` are the established null
			//checks — the null side keeps its raw sentinel bits and takes
			//the identity/sentinel comparison path.
			if ((lk == NK_String) != (rk == NK_String) && !lNull && !rNull)
			{
				m_Env.Log(CLL_Error, sn.Location(),
					"cannot compare string with a non-string operand "
					"(only null is allowed as the other side).");
				return;
			}

			//Phase 13: function handles support content equality only
			//(==/!=), and only against another function handle or null.
			//Without this gate codegen's dispatch would silently fall to
			//the integer variant comparing raw heap indices.
			bool lFunc = IsFuncTypeDecl(L);
			bool rFunc = IsFuncTypeDecl(R);
			if (lFunc || rFunc)
			{
				bool bEq = op == SnBinaryExpr::OP_Equal
					|| op == SnBinaryExpr::OP_NotEqual;
				if (!bEq)
				{
					m_Env.Log(CLL_Error, sn.Location(),
						"function values cannot be ordered; only == and != "
						"are supported.");
					return;
				}
				if (!((lFunc && rFunc) || (lFunc && rNull)
					|| (rFunc && lNull)))
				{
					m_Env.Log(CLL_Error, sn.Location(),
						"a function value can only be compared with a "
						"function value or null.");
					return;
				}
			}

			//0.7.3 B D8: array values support identity comparison only
			//(==/!=), and only against another array or null. Without
			//this gate relational mixes (`a < b`) and scalar mixes
			//(`a == 5`) compile and silently compare the raw heap
			//handle against the operand. Mirrors the function-handle
			//gate above; null keeps the sentinel comparison path.
			bool lArr = lk == NK_ArrayTypeToken;
			bool rArr = rk == NK_ArrayTypeToken;
			if (lArr || rArr)
			{
				bool bEq = op == SnBinaryExpr::OP_Equal
					|| op == SnBinaryExpr::OP_NotEqual;
				if (!bEq || !((lArr && rArr) || (lArr && rNull)
					|| (rArr && lNull)))
				{
					m_Env.Log(CLL_Error, sn.Location(),
						"array values support identity comparison only "
						"(==/!=), and only against another array or "
						"null.");
					return;
				}
			}

			//Symmetric int/float promotion (Phase 8e-8 mechanism) extended
			//to comparisons. Prerequisite the arithmetic branch does not
			//have: BOTH operands numeric and neither a null literal —
			//class/enum/null pairs stay on their existing identity or
			//sentinel paths, and wrapping them (as the arithmetic branch
			//would) would break e.g. class identity equality.
			bool lNum = lk == NK_Int32 || lk == NK_Float;
			bool rNum = rk == NK_Int32 || rk == NK_Float;
			if (lNum && rNum && !lNull && !rNull && lk != rk)
			{
				SnField* T_promote =
					(lk == NK_Float || rk == NK_Float)
					? SnBuiltinDataType::InstanceOf(NK_Float)
					: SnBuiltinDataType::InstanceOf(NK_Int32);
				auto it = sn.Children().begin();
				auto& leftExpr = static_cast<SnExpression&>(*it);
				TypeCastInfo leftCI(leftExpr.EvalDataType(), T_promote);
				FixupExprType(it, leftCI);
				++it;
				auto& rightExpr = static_cast<SnExpression&>(*it);
				TypeCastInfo rightCI(rightExpr.EvalDataType(), T_promote);
				FixupExprType(it, rightCI);
			}
		}
		//Short-circuit hardening (2026-08-31): logical operands feed
		//OP_JumpIfNot, which reads one int32 — the same policy as
		//statement conditions (CheckIntCondition in
		//StatementResolver.hpp; widen both together). Without this
		//gate a float/string operand is read as raw bits, giving
		//garbage truthiness.
		if (!isCompare)
		{
			auto bop = sn.Op();
			const char* szOp = bop == SnBinaryExpr::OP_LogicalAnd
				? "&&" : bop == SnBinaryExpr::OP_LogicalOr
				? "||" : "!";
			const char* szShape = sn.Right()
				? "int operands" : "an int operand";
			auto* pLT = sn.Left()->EvalDataType();
			if (pLT && pLT->Kind() != NK_Int32)
			{
				m_Env.Log(CLL_Error, sn.Left()->Location(),
					"operator '%s' requires %s, got \"%s\".",
					szOp, szShape, pLT->ToString().c_str());
				return;
			}
			if (sn.Right())
			{
				auto* pRT = sn.Right()->EvalDataType();
				if (pRT && pRT->Kind() != NK_Int32)
				{
					m_Env.Log(CLL_Error, sn.Right()->Location(),
						"operator '%s' requires %s, got \"%s\".",
						szOp, szShape, pRT->ToString().c_str());
					return;
				}
			}
		}
		auto* intType = SnBuiltinDataType::InstanceOf(NK_Int32);
		sn.EvalDataType(intType);
	}
	else
	{
		//Phase 8e-8: symmetric arithmetic promotion.
		//Both operands are promoted to the wider type (int<float). For string
		//only OP_Add is valid (concat); other ops on string are rejected here.
		//Each operand is wrapped in SnCastExpr if its type differs from T_result
		//so that codegen sees uniform operand types matching bin.EvalDataType().
		//Phase 11 Q4: null is only meaningful through the comparison identity
		//path above; arithmetic/concat with null is a compile error. (Before
		//the null-sentinel fix it silently produced "0" concatenations; after
		//it, an unwrapped raw 0.)
		if (sn.Left()->ContainFlags(NF_NullLiteral)
			|| (sn.Right() && sn.Right()->ContainFlags(NF_NullLiteral)))
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"null is not a valid arithmetic operand.");
			return;
		}
		auto* L = sn.Left()->EvalDataType();
		auto* R = sn.Right() ? sn.Right()->EvalDataType() : nullptr;
		NodeKind lk = L ? L->Kind() : NK_Int32;
		NodeKind rk = R ? R->Kind() : NK_Int32;

		SnField* T_result = nullptr;
		if (lk == NK_String || rk == NK_String)
		{
			if (op != SnBinaryExpr::OP_Add)
			{
				m_Env.Log(CLL_Error, sn.Location(),
					"operator not supported on string.");
				return;
			}
			//Phase 8e-9a: allow mixed (e.g. int + string). The non-string
			//operand is wrapped in SnCastExpr below; VmBackend.cpp:951
			//emits OP_Int32_to_str / OP_Float_to_str for the conversion.
			//Then OP_Concat_str concatenates the two string indices.
			T_result = SnBuiltinDataType::InstanceOf(NK_String);
		}
		else if (lk == NK_Float || rk == NK_Float)
		{
			T_result = SnBuiltinDataType::InstanceOf(NK_Float);
		}
		else
		{
			T_result = SnBuiltinDataType::InstanceOf(NK_Int32);
		}
		sn.EvalDataType(T_result);

		//Wrap each operand (in-place via FixupExprType) if its type differs
		//from T_result. After wrap, sn.Children()[0]/[1] hold the (possibly
		//cast) expressions; sn.Left()/Right() are stale but unused by codegen.
		auto it = sn.Children().begin();
		auto& leftExpr = static_cast<SnExpression&>(*it);
		TypeCastInfo leftCI(leftExpr.EvalDataType(), T_result);
		FixupExprType(it, leftCI);
		if (sn.Right())
		{
			++it;
			auto& rightExpr = static_cast<SnExpression&>(*it);
			TypeCastInfo rightCI(rightExpr.EvalDataType(), T_result);
			FixupExprType(it, rightCI);
		}
	}
	sn.AddFlags(NF_Resolved);
}

} //namespace nlang
