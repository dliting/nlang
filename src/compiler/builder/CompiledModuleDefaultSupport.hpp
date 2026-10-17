#pragma once
//Reconstruction of imported default-value expressions (v2.3). Split from
//CompiledModuleNodeBuilder.hpp, which sat at the 500-line source-size
//cap: the literal stubs (Option B Step 4) and the ExprDesc rebuild ride
//the same hand-off (CompiledFunction record → synthesized AST), so the
//family lives together. Free functions — they touch no builder state.
#include "CompiledModuleStubSupport.hpp"
#include "SnMisc.h"
#include "SnData.h"
#include "SnExpressions.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/ExprDesc.h"
#include <nlang/runtime/RnTypes.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang
{

//v2.3 reject for a malformed ExprDesc stream. A valid producer never
//writes one (the writer validates shapes as it encodes), so this
//surfaces corrupt/hand-made images, not source errors.
[[noreturn]] inline void ThrowMalformedDefault(
	const std::string &funcName, const char *what)
{
	throw std::runtime_error("imported function '" + funcName
		+ "' has a malformed default-value expression (" + what + ")");
}

//Literal arms of SynthDefaultExpr (Option B Step 4): reconstruct an
//SnLiteralExpr from the scalar DefaultValueDesc channels. Returns
//nullptr for anything else. For RTK_String, the producer's
//stringConstants index is resolved against cm.stringConstants and the
//content is embedded directly — VmBackend's codegen re-interns it into
//the consumer's pool at call-site emission. The RTTI ctor flags the
//node NF_Imported/NF_Resolved (parse-time parity).
inline SnExpression *SynthScalarDefault(const DefaultValueDesc &dv,
	const CompiledModule &cm, const ISourceLocation &loc)
{
	switch (dv.tag)
	{
		case RTK_Null:
		{
			auto *lit = new SnLiteralExpr(*RnInt32::Instance(), 0, loc);
			lit->AddFlags(NF_NullLiteral);
			return lit;
		}
		case RTK_Int32:
			return new SnLiteralExpr(*RnInt32::Instance(),
				static_cast<int32_t>(dv.intValue), loc);
		case RTK_Long:
			return new SnLiteralExpr(*RnLong::Instance(),
				static_cast<int64>(dv.longValue), loc);
		case RTK_ULong:
			return new SnLiteralExpr(*RnULong::Instance(),
				static_cast<uint64>(dv.longValue), loc);
		case RTK_Double:
			return new SnLiteralExpr(*RnDouble::Instance(),
				dv.doubleValue, loc);
		case RTK_Float:
			return new SnLiteralExpr(*RnFloat::Instance(),
				dv.floatValue, loc);
		case RTK_String:
		{
			if (dv.stringIdx >= cm.stringConstants.size())
				return nullptr;  //malformed — skip default
			const std::string &content = cm.stringConstants[dv.stringIdx];
			return new SnLiteralExpr(*RnString::Instance(),
				new std::string(content), loc);
		}
		default:
			return nullptr;
	}
}

//Reconstruct an Sn expression for a formal default from the serialized
//DefaultValueDesc. Returns nullptr when the formal has no default
//(RTK_Void / unknown tag).
//
//RTK_Expr (v2.3) rebuilds the full tree via SynthExprDescNode. Literal
//results come back pre-resolved; the compound v2.3 nodes are NOT — they
//resolve in ResolveFunctionFormals like any hand-written default.
inline SnExpression *SynthDefaultExpr(const DefaultValueDesc &dv,
	const CompiledModule &cm, const std::string &funcName,
	uint16_t paramIdx, const std::vector<std::string> &stubFormalNames,
	const ISourceLocation &loc);

//Rebuild one node of an UNRESOLVED Sn expression tree from an ExprDesc
//stream (grammar in ExprDesc.h). Formal references mint plain
//identifiers — ResolveFunctionFormals binds them to the stub formals by
//name; calls mint module-qualified member invokes — the same shape a
//hand-written `lib.helper(...)` parses into, bound through the
//imported-module registry.
inline SnExpression *SynthExprDescNode(ExprDescCursor &cur,
	const CompiledModule &cm, const std::string &funcName,
	const std::vector<std::string> &stubFormalNames,
	const ISourceLocation &loc, int depth);

//Literal arms of the node rebuild: a tagged scalar payload straight off
//the cursor (same kind set as the writer's EncodeLiteralDesc).
inline SnExpression *SynthExprDescLiteral(uint8_t tag,
	ExprDescCursor &cur, const CompiledModule &cm,
	const std::string &funcName, const ISourceLocation &loc)
{
	switch (tag)
	{
		case RTK_Int32:
		{
			uint32_t v = 0;
			if (!cur.U32(v)) ThrowMalformedDefault(funcName, "truncated stream");
			return new SnLiteralExpr(*RnInt32::Instance(),
				static_cast<int32_t>(v), loc);
		}
		case RTK_Long:
		{
			int64_t v = 0;
			if (!cur.I64(v)) ThrowMalformedDefault(funcName, "truncated stream");
			return new SnLiteralExpr(*RnLong::Instance(),
				static_cast<int64>(v), loc);
		}
		case RTK_ULong:
		{
			int64_t v = 0;
			if (!cur.I64(v)) ThrowMalformedDefault(funcName, "truncated stream");
			return new SnLiteralExpr(*RnULong::Instance(),
				static_cast<uint64>(v), loc);
		}
		case RTK_Double:
		{
			double v = 0.0;
			if (!cur.F64(v)) ThrowMalformedDefault(funcName, "truncated stream");
			return new SnLiteralExpr(*RnDouble::Instance(), v, loc);
		}
		case RTK_Float:
		{
			float v = 0.0f;
			if (!cur.F32(v)) ThrowMalformedDefault(funcName, "truncated stream");
			return new SnLiteralExpr(*RnFloat::Instance(), v, loc);
		}
		case RTK_String:
		{
			uint32_t idx = 0;
			if (!cur.U32(idx))
				ThrowMalformedDefault(funcName, "truncated stream");
			if (idx >= cm.stringConstants.size())
				ThrowMalformedDefault(funcName, "string index out of range");
			return new SnLiteralExpr(*RnString::Instance(),
				new std::string(cm.stringConstants[idx]), loc);
		}
		case RTK_Null:
		{
			auto *lit = new SnLiteralExpr(*RnInt32::Instance(), 0, loc);
			lit->AddFlags(NF_NullLiteral);
			return lit;
		}
		default:
			return nullptr;
	}
}

//ED_Call arm: mint the outer identifier chain of the dotted module path,
//left to right (single segment = the common `lib.helper()` shape).
inline SnExpression *SynthModuleChainExpr(const std::string &modulePath,
	const ISourceLocation &loc)
{
	SnExpression *pChain = nullptr;
	size_t segStart = 0;
	while (segStart != std::string::npos)
	{
		const size_t dot = modulePath.find('.', segStart);
		const std::string seg = dot == std::string::npos
			? modulePath.substr(segStart)
			: modulePath.substr(segStart, dot - segStart);
		auto *pSeg = new SnIdentifierExpr(new std::string(seg), loc);
		pChain = pChain
			? static_cast<SnExpression *>(
				new SnMemberExpr(pChain, pSeg, loc))
			: pSeg;
		segStart = dot == std::string::npos ? std::string::npos : dot + 1;
	}
	return pChain;
}

//ED_Call arm: validate the producing-module target against cm's
//function table, then mint `modulePath.leaf(args...)` — the AST the
//parser builds for that source spelling, resolved by the normal
//module-qualified call path.
inline SnExpression *SynthExprDescCall(ExprDescCursor &cur,
	const CompiledModule &cm, const std::string &funcName,
	const std::vector<std::string> &stubFormalNames,
	const ISourceLocation &loc, int depth)
{
	uint16_t keyLen = 0;
	if (!cur.U16(keyLen) || keyLen == 0)
		ThrowMalformedDefault(funcName, "truncated call");
	std::vector<char> keyBuf(keyLen);
	if (!cur.Bytes(keyBuf.data(), keyLen))
		ThrowMalformedDefault(funcName, "truncated call key");
	uint32_t paramCount = 0;
	uint8_t argCount = 0;
	if (!cur.U32(paramCount) || !cur.U8(argCount))
		ThrowMalformedDefault(funcName, "truncated call");
	std::string key(keyBuf.data(), keyLen);
	const int targetIdx = cm.FindFunction(key);
	if (targetIdx < 0 || cm.functions[targetIdx].paramCount != paramCount
		|| argCount != paramCount)
		throw std::runtime_error("imported function '" + funcName
			+ "' has a default expression calling '" + key
			+ "', which is not a function of the producing module");
	const size_t lastDot = key.find_last_of('.');
	if (lastDot == std::string::npos || lastDot == 0
		|| lastDot + 1 == key.size())
		ThrowMalformedDefault(funcName, "unqualified call key");

	auto *pArgs = new PtrList<SnExpression>();
	for (uint8_t i = 0; i < argCount; ++i)
		pArgs->push_back(SynthExprDescNode(cur, cm, funcName,
			stubFormalNames, loc, depth + 1));
	auto *pInvoke = new SnInvokeExpr(new std::string(key.substr(lastDot + 1)),
		UniquePtrList<SnExpression>(pArgs), loc);
	return new SnMemberExpr(
		SynthModuleChainExpr(key.substr(0, lastDot), loc),
		pInvoke, loc);
}

//The tag dispatch (see SynthExprDescNode above).
inline SnExpression *SynthExprDescNode(ExprDescCursor &cur,
	const CompiledModule &cm, const std::string &funcName,
	const std::vector<std::string> &stubFormalNames,
	const ISourceLocation &loc, int depth)
{
	if (depth > kMaxExprDescDepth)
		ThrowMalformedDefault(funcName, "expression too deep");
	uint8_t tag = 0;
	if (!cur.U8(tag))
		ThrowMalformedDefault(funcName, "truncated stream");
	if (SnExpression *pLit = SynthExprDescLiteral(tag, cur, cm, funcName,
			loc))
		return pLit;
	switch (tag)
	{
		case ED_FormalRef:
		{
			uint8_t idx = 0;
			if (!cur.U8(idx))
				ThrowMalformedDefault(funcName, "truncated stream");
			if (idx >= stubFormalNames.size() || stubFormalNames[idx].empty())
				ThrowMalformedDefault(funcName, "formal index out of range");
			return new SnIdentifierExpr(
				new std::string(stubFormalNames[idx]), loc);
		}
		case ED_Neg:
			return new SnBinaryExpr(SnBinaryExpr::OP_Neg,
				SynthExprDescNode(cur, cm, funcName, stubFormalNames,
					loc, depth + 1),
				loc);
		case ED_Binary:
		{
			uint8_t opCode = 0;
			if (!cur.U8(opCode) || opCode > EDB_Mod)
				ThrowMalformedDefault(funcName, "bad binary operator");
			SnExpression *pLeft = SynthExprDescNode(cur, cm, funcName,
				stubFormalNames, loc, depth + 1);
			SnExpression *pRight = SynthExprDescNode(cur, cm, funcName,
				stubFormalNames, loc, depth + 1);
			static const SnBinaryExpr::Operator kOps[] = {
				SnBinaryExpr::OP_Add, SnBinaryExpr::OP_Sub,
				SnBinaryExpr::OP_Mul, SnBinaryExpr::OP_Div,
				SnBinaryExpr::OP_Mod,
			};
			return new SnBinaryExpr(kOps[opCode], pLeft, pRight, loc);
		}
		case ED_Call:
			return SynthExprDescCall(cur, cm, funcName,
				stubFormalNames, loc, depth);
		default:
			ThrowMalformedDefault(funcName, "unknown node tag");
	}
}

//The RTK_Expr dispatch (see SynthDefaultExpr above).
inline SnExpression *SynthDefaultExpr(const DefaultValueDesc &dv,
	const CompiledModule &cm, const std::string &funcName,
	uint16_t paramIdx, const std::vector<std::string> &stubFormalNames,
	const ISourceLocation &loc)
{
	if (dv.tag == RTK_Expr)
	{
		ExprDescCursor cur(dv.exprBytes);
		SnExpression *pExpr = SynthExprDescNode(cur, cm, funcName,
			stubFormalNames, loc, 0);
		if (!cur.AtEnd())
			ThrowMalformedDefault(funcName, "trailing bytes");
		return pExpr;
	}
	if (dv.tag == RTK_Unfoldable)
	{
		//Producer's original default was outside the ExprDesc subset
		//(e.g. a cast or a bool literal). Cross-module callers can't
		//reconstruct it; reject at consumer-side stub construction
		//(LoadImports catches + logs as compile error).
		throw std::runtime_error(
			"imported function '" + funcName + "' has a "
			"non-constant-foldable default for parameter " +
			std::to_string(static_cast<unsigned long>(paramIdx)) +
			"; cross-module defaults must be literal "
			"(int/float/string/null/negative)");
	}
	return SynthScalarDefault(dv, cm, loc);
}

} //namespace nlang
