#pragma once
/*---
    CompiledModuleCtorStub.hpp — imported 构造器（method-layout 函数记录）
    的 AST stub 构建（0.9.11 D10/O2）。从 CompiledModuleNodeBuilder.hpp 拆出
    （源尺寸守卫）；所依赖的自由助手（SynthTypeExprFromDesc /
    SynthDefaultExpr / LeafNameOfKey）住在同族支持头文件里。
---*/
#include "CompiledModuleDefaultSupport.hpp"
#include "CompiledModuleStubSupport.hpp"
#include "SnData.h"
#include "SnExpressions.h"
#include "SnStatements.h"
#include "nlang/vm/CompiledModule.h"
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang
{

//Build a SnFunction stub for an imported METHOD-layout record — the
//constructor member of an imported class/struct. The method layout differs
//from the free-function layout CreateFunctionStub consumes: paramCount
//counts the hidden __this (paramCount == formals + 1), the locals table
//lists __this as its FIRST isParam entry, and paramTypeDescs/defaultValues
//are per-formal (index 0 == first visible parameter). The stub therefore
//mints formals = paramCount - 1 params, named from the locals entries after
//__this, with defaults/descriptors aligned from 0 — the same shapes the
//parser emits for a hand-written `C(int x)` member. A record that cannot
//name a __this slot is a corrupt image: refusing it beats minting a stub
//whose call frame is one slot off (the walker/codegen claim split would
//trip the observedPeakCursor tripwire far from the cause).
inline SnFunction *CreateMethodStub(const CompiledFunction &cf,
	const CompiledModule &cm, const ISourceLocation &loc)
{
	if (cf.paramCount < 1)
		throw std::runtime_error(
			"imported constructor '" + cf.name
			+ "' has an inconsistent frame layout");
	const uint16_t formals = cf.paramCount - 1;

	//Formal names: the isParam locals entries minus the leading __this
	//(named-argument binding matches actuals to formals by name).
	std::vector<std::string> paramNames;
	for (const auto &ld : cf.locals)
		if (ld.isParam)
			paramNames.push_back(ld.name);
	if (!paramNames.empty())
		paramNames.erase(paramNames.begin());
	//Placeholder fallback mirrors CreateFunctionStub — stub minting never
	//rejects a module over naming.
	std::vector<std::string> stubFormalNames(formals);
	for (uint16_t i = 0; i < formals; ++i)
		stubFormalNames[i] =
			(i < paramNames.size() && !paramNames[i].empty())
				? paramNames[i]
				: "p" + std::to_string(i);

	//Return type: constructor records are RTK_Void → no return type
	//(HasReturn() == false, the D1 constructor shape).
	SnFieldExpr *pRetType = nullptr;
	if (cf.returnTypeKind != RTK_Void)
		pRetType = SynthTypeExprFromDesc(cf.returnTypeDesc, cm, loc);

	auto *pParams = new PtrList<SnFormalParam>();
	for (uint16_t i = 0; i < formals; ++i)
	{
		auto paramName = new std::string(stubFormalNames[i]);
		NodeBits paramFlags = NF_NONE;
		SnFieldExpr *pParamType;
		if (i < cf.paramTypeDescs.size())
		{
			const ParamTypeDesc &ptd = cf.paramTypeDescs[i];
			if (ptd.flags & PTDF_Out)
				paramFlags |= NF_Out;
			pParamType = SynthTypeExprFromDesc(ptd.type, cm, loc);
		}
		else
		{
			pParamType = new SnIdentifierExpr(NK_Int32, loc);
		}
		SnExpression *pDefault = nullptr;
		if (i < cf.defaultValues.size())
			pDefault = SynthDefaultExpr(cf.defaultValues[i], cm,
				cf.name, i, stubFormalNames, loc);
		auto *pParam = new SnFormalParam(paramFlags, pParamType, paramName,
			pDefault, loc);
		pParams->push_back(pParam);
	}

	//Leaf name (phase 5 key vs. stub-name split): the resolver binds the
	//ctor member by the caller's bare callee name.
	auto pName = new std::string(LeafNameOfKey(cf.name));
	auto *pFunc = new SnFunction(FA_Public, NF_Data, pRetType, pName,
		pParams, loc);
	pFunc->AddFlags(NF_Imported);   //R6-2
	if (cf.isStatic)
		pFunc->AddFlags(NF_Static);
	return pFunc;
}

} //namespace nlang
