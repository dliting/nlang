#pragma once
/*---
    CompiledModuleCtorStub.hpp — imported 函数记录的 AST stub 构建：自由
    函数/静态布局（CreateFunctionStub，0.13.1a Task 7 起自 StubMembers 迁
    入）与 method 布局（构造器与实例方法，0.9.11 D10/O2；0.9.12 起实例方
    法共用）。从 CompiledModuleNodeBuilder.hpp 拆出（源尺寸守卫）；所依赖
    的自由助手（SynthTypeExprFromDesc / SynthDefaultExpr / LeafNameOfKey）
    住在同族支持头文件里。
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

//Build a SnFunction stub for an imported free-function or class/interface
//static record. The stub has no Body() — VmBackend.RegisterFunctions and
//GenerateAllBytecode already skip bodyless functions, and the NF_Imported
//flag adds an explicit IsImported() gate for safety. `access` is the
//member's declared access (0.9.12: class statics carry it on the wire);
//free functions and interface statics mint FA_Public at the call site.
//`producerIdx` is the record's index in the producer's own function
//table (0.10.14, clamped through ProducerIdxClamp) — it rides the stub
//to the cross-unit slot record; no default value, every mint site
//states it explicitly.
inline SnFunction *CreateFunctionStub(const CompiledFunction &cf,
	const CompiledModule &cm, const ISourceLocation &loc,
	FieldAccessType access, uint16_t producerIdx,
	const std::vector<std::string> *typeParamNames = nullptr)
{
	//Return type. RTK_Void → no return type (SnFunction::HasReturn() == false).
	//v1.12: the serialized descriptor supplies the true type; the
	//NonSerialized sentinel (func/interface returns) degrades to the
	//int32 placeholder.
	SnFieldExpr *pRetType = nullptr;
	if (cf.returnTypeKind != RTK_Void)
		pRetType = SynthTypeExprFromDesc(cf.returnTypeDesc, cm, loc,
			typeParamNames);

	//Synthesize paramCount SnFormalParam nodes. v1.12: each formal's true
	//type and out flag come from the serialized descriptors (free
	//functions carry exactly paramCount of them — methods never reach
	//stub construction, the methodOrCtorIndices filter owns those).
	//Option B: if cf.defaultValues[i] carries a constant-foldable default,
	//attach the reconstructed SnLiteralExpr to the stub formal so the
	//resolver can apply it at consumer call sites.
	//Use raw PtrList<SnFormalParam>* — SnFunction takes UniquePtrList by
	//value, whose inner_collection* constructor assumes ownership and
	//deletes the source list.
	auto *pParams = new PtrList<SnFormalParam>();
	//Formal names from the producer's frame-layout locals table
	//(ModuleSaver serializes every AllocLocal name with its isParam
	//flag; for free functions those entries are exactly the params,
	//in declaration order). Named-argument binding (BindNamedActual)
	//matches actuals to formals by name, so the former "p0"/"p1"
	//placeholders made every cross-module named call unbindable.
	//Missing/misaligned entries fall back to the placeholders —
	//stub construction never rejects a module over naming.
	std::vector<std::string> formalNames;
	for (const auto &ld : cf.locals)
		if (ld.isParam)
			formalNames.push_back(ld.name);
	//Effective per-formal names (locals entries with the placeholder
	//fallback), shared by the param nodes and the v2.3 ExprDesc
	//formal-reference rebuild — a rebuilt `b = a * 2` must name the
	//formals exactly as the stub's own params do.
	std::vector<std::string> stubFormalNames(cf.paramCount);
	for (uint16_t i = 0; i < cf.paramCount; ++i)
		stubFormalNames[i] =
			(i < formalNames.size() && !formalNames[i].empty())
				? formalNames[i]
				: "p" + std::to_string(i);
	for (uint16_t i = 0; i < cf.paramCount; ++i)
	{
		auto paramName = new std::string(stubFormalNames[i]);
		NodeBits paramFlags = NF_NONE;
		SnFieldExpr *pParamType;
		if (i < cf.paramTypeDescs.size())
		{
			const ParamTypeDesc &ptd = cf.paramTypeDescs[i];
			if (ptd.flags & PTDF_Out)
				paramFlags |= NF_Out;
			pParamType = SynthTypeExprFromDesc(ptd.type, cm, loc,
				typeParamNames);
		}
		else
		{
			pParamType = PreboundLeafExpr(NK_Int32, loc);
		}
		SnExpression *pDefault = nullptr;
		if (i < cf.defaultValues.size())
			pDefault = SynthDefaultExpr(cf.defaultValues[i], cm,
				cf.name, i, stubFormalNames, loc);
		auto *pParam = new SnFormalParam(paramFlags, pParamType, paramName,
			pDefault, loc);
		pParams->push_back(pParam);
	}

	//Mint the function name as a heap string (SnFunction takes ownership).
	//Leaf name: see LeafNameOfKey (phase 5 key vs. stub-name split).
	auto pName = new std::string(LeafNameOfKey(cf.name));

	auto *pFunc = new SnFunction(access, NF_Data, pRetType, pName,
		pParams, loc);
	//R6-2: explicitly add NF_Imported. SnFunction's constructor does
	//not auto-set this flag (unlike SnIdentifierExpr). Without it,
	//RegisterStructs/Classes/.../PopulateClassMethods' IsImported()
	//gates would not skip these stubs.
	pFunc->AddFlags(NF_Imported);
	//0.10.14: the producer table index rides the stub to the
	//cross-unit slot record (see SnFunction::m_iProducerFuncIdx).
	pFunc->m_iProducerFuncIdx = producerIdx;
	//0.9.10 D5: the static flag rides the record — the resolver's
	//receiver-namespace split (MatchesReceiverForm) and the emitter's
	//slot arity (FunctionSlotFor: statics take no this slot) both key
	//on it.
	if (cf.isStatic)
		pFunc->AddFlags(NF_Static);
	//0.13.1a Task 7: a generic free function rebuilds as a generic
	//declaration — the names make IsGenericDeclaration() true, so the
	//consumer's call-site inference reads the stub's own formals (the
	//class-stub mirror: members get T synthesis via typeParamNames, the
	//class decl carries the genericity; a free function IS the decl).
	if (typeParamNames != nullptr && !typeParamNames->empty())
		pFunc->SetTypeParamNames(*typeParamNames);

	return pFunc;
}

//Build a SnFunction stub for an imported METHOD-layout record — a
//constructor or an instance-method member of an imported class/struct
//(0.9.12: instance methods ride the same layout). The method layout
//differs from the free-function layout CreateFunctionStub consumes:
//paramCount counts the hidden __this (paramCount == formals + 1), the
//locals table lists __this as its FIRST isParam entry, and
//paramTypeDescs/defaultValues are per-formal (index 0 == first visible
//parameter). The stub therefore mints formals = paramCount - 1 params,
//named from the locals entries after __this, with defaults/descriptors
//aligned from 0 — the same shapes the parser emits for a hand-written
//`C(int x)` member. `access` is the member's declared access (v2.7 wire).
//A record that cannot name a __this slot is a corrupt image: refusing it
//beats minting a stub whose call frame is one slot off (the
//walker/codegen claim split would trip the observedPeakCursor tripwire
//far from the cause).
//`producerIdx` (0.10.14): the record's index in the producer's own
//function table, clamped by the caller through ProducerIdxClamp — it
//rides the stub to the cross-unit slot record. No default value: every
//mint site states it explicitly.
inline SnFunction *CreateMethodStub(const CompiledFunction &cf,
	const CompiledModule &cm, const ISourceLocation &loc,
	FieldAccessType access, uint16_t producerIdx,
	const std::vector<std::string> *typeParamNames = nullptr)
{
	if (cf.paramCount < 1)
		throw std::runtime_error(
			"imported member '" + cf.name
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
		pRetType = SynthTypeExprFromDesc(cf.returnTypeDesc, cm, loc,
			typeParamNames);

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
			pParamType = SynthTypeExprFromDesc(ptd.type, cm, loc,
				typeParamNames);
		}
		else
		{
			pParamType = PreboundLeafExpr(NK_Int32, loc);
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
	//member by the caller's bare callee name.
	auto pName = new std::string(LeafNameOfKey(cf.name));
	auto *pFunc = new SnFunction(access, NF_Data, pRetType, pName,
		pParams, loc);
	pFunc->AddFlags(NF_Imported);   //R6-2
	//0.10.14: the producer table index rides the stub to the
	//cross-unit slot record (see SnFunction::m_iProducerFuncIdx).
	pFunc->m_iProducerFuncIdx = producerIdx;
	if (cf.isStatic)
		pFunc->AddFlags(NF_Static);
	return pFunc;
}

} //namespace nlang
