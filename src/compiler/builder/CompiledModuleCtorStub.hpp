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

//Effective per-formal names for a function-record stub: the producer's
//isParam locals entries (ModuleSaver serializes every AllocLocal name
//with its isParam flag, in declaration order) — minus the leading __this
//for method-layout records (bDropThis) — with the "p<i>" placeholder
//fallback. Named-argument binding (BindNamedActual) matches actuals to
//formals by name, so the former bare placeholders made every cross-module
//named call unbindable; missing/misaligned entries degrade to
//placeholders — stub construction never rejects a module over naming.
//The effective names are shared by the param nodes and the v2.3
//ExprDesc formal-reference rebuild — a rebuilt `b = a * 2` must name
//the formals exactly as the stub's own params do.
inline std::vector<std::string> StubFormalNamesOf(const CompiledFunction &cf,
	uint16_t formals, bool bDropThis)
{
	std::vector<std::string> paramNames;
	for (const auto &ld : cf.locals)
		if (ld.isParam)
			paramNames.push_back(ld.name);
	if (bDropThis && !paramNames.empty())
		paramNames.erase(paramNames.begin());
	std::vector<std::string> stubFormalNames(formals);
	for (uint16_t i = 0; i < formals; ++i)
		stubFormalNames[i] =
			(i < paramNames.size() && !paramNames[i].empty())
				? paramNames[i]
				: "p" + std::to_string(i);
	return stubFormalNames;
}

//One stub formal per effective name, typed from the serialized
//descriptors (v1.12: each formal's true type and out flag come from
//ParamTypeDesc — PTDF_Out → NF_Out; a missing descriptor degrades to
//the int32 placeholder) and carrying the reconstructed constant-foldable
//default when the record has one (Option B — the resolver applies it
//at consumer call sites). Template over the record shape: CompiledFunction
//and CompiledInterfaceMethod carry the same paramTypeDescs/defaultValues
//fields. Use raw PtrList<SnFormalParam>* — SnFunction takes
//UniquePtrList by value, whose inner_collection* constructor assumes
//ownership and deletes the source list.
template <typename FunctionRecord>
inline PtrList<SnFormalParam> *BuildStubFormalParamNodes(
	const FunctionRecord &rec, const CompiledModule &cm,
	const std::vector<std::string> &stubFormalNames,
	const ISourceLocation &loc,
	const std::vector<std::string> *typeParamNames)
{
	auto *pParams = new PtrList<SnFormalParam>();
	for (size_t i = 0; i < stubFormalNames.size(); ++i)
	{
		auto paramName = new std::string(stubFormalNames[i]);
		NodeBits paramFlags = NF_NONE;
		SnFieldExpr *pParamType;
		if (i < rec.paramTypeDescs.size())
		{
			const ParamTypeDesc &ptd = rec.paramTypeDescs[i];
			if (ptd.flags & PTDF_Out)
				paramFlags |= NF_Out;
			pParamType = SynthTypeExprFromDesc(ptd.type, cm, loc,
				typeParamNames);
			//v2.21: the & and const marks re-stamp on the formal AND its
			//type node — the post-resolution state of a source-declared
			//formal (the grammar puts & on the type node, resolution
			//propagates it onto the descriptor; const sits on the formal).
			if (ptd.flags & PTDF_Ref)
			{
				paramFlags |= NF_Ref;
				pParamType->AddFlags(NF_Ref);
			}
			if (ptd.flags & PTDF_Const)
				paramFlags |= NF_Const;
		}
		else
		{
			pParamType = PreboundLeafExpr(NK_Int32, loc);
		}
		SnExpression *pDefault = nullptr;
		if (i < rec.defaultValues.size())
			pDefault = SynthDefaultExpr(rec.defaultValues[i], cm,
				rec.name, static_cast<uint16_t>(i), stubFormalNames, loc);
		auto *pParam = new SnFormalParam(paramFlags, pParamType, paramName,
			pDefault, loc);
		pParams->push_back(pParam);
	}
	return pParams;
}

//The flag set every function-record stub carries: NF_Imported (R6-2 —
//SnFunction's constructor does not auto-set it, unlike SnIdentifierExpr;
//without it RegisterStructs/Classes/.../PopulateClassMethods' IsImported()
//gates would not skip these stubs), the producer's table index (0.10.14 —
//rides the stub to the cross-unit slot record, see
//SnFunction::m_iProducerFuncIdx), and the static flag (0.9.10 D5 — the
//resolver's receiver-namespace split MatchesReceiverForm and the
//emitter's slot arity FunctionSlotFor both key on it). A non-empty
//typeParamNames list makes the stub a generic declaration (0.13.1a
//Task 7 — the names make IsGenericDeclaration() true, so the consumer's
//call-site inference reads the stub's own formals); CreateMethodStub
//passes nullptr: a method is never generic on its own, the class decl
//carries the genericity.
inline void ApplyStubFunctionFlags(SnFunction *pFunc,
	const CompiledFunction &cf, uint16_t producerIdx,
	const std::vector<std::string> *typeParamNames)
{
	pFunc->AddFlags(NF_Imported);   //R6-2
	pFunc->m_iProducerFuncIdx = producerIdx;
	if (cf.isStatic)
		pFunc->AddFlags(NF_Static);
	if (typeParamNames != nullptr && !typeParamNames->empty())
		pFunc->SetTypeParamNames(*typeParamNames);
}

//Build a SnFunction stub for an imported free-function or class/interface
//static record. The stub has no Body() — VmBackend.RegisterFunctions and
//GenerateAllBytecode already skip bodyless functions, and the NF_Imported
//flag adds an explicit IsImported() gate for safety. `access` is the
//member's declared access (0.9.12: class statics carry it on the wire);
//free functions and interface statics create FA_Public at the call site.
//`producerIdx` is the record's index in the producer's own function
//table (0.10.14, clamped through ProducerIdxClamp) — it rides the stub
//to the cross-unit slot record; no default value, every creation site
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

	//paramCount SnFormalParam nodes (free functions carry exactly
	//paramCount of them — methods never reach stub construction, the
	//methodOrCtorIndices filter owns those).
	auto stubFormalNames = StubFormalNamesOf(cf, cf.paramCount, false);
	auto *pParams = BuildStubFormalParamNodes(cf, cm, stubFormalNames,
		loc, typeParamNames);

	//Create the function name as a heap string (SnFunction takes ownership).
	//Leaf name: see LeafNameOfKey (phase 5 key vs. stub-name split).
	auto pName = new std::string(LeafNameOfKey(cf.name));

	auto *pFunc = new SnFunction(access, NF_Data, pRetType, pName,
		pParams, loc);
	ApplyStubFunctionFlags(pFunc, cf, producerIdx, typeParamNames);
	return pFunc;
}

//Build a SnFunction stub for an imported METHOD-layout record — a
//constructor or an instance-method member of an imported class/struct
//(0.9.12: instance methods ride the same layout). The method layout
//differs from the free-function layout CreateFunctionStub consumes:
//paramCount counts the hidden __this (paramCount == formals + 1), the
//locals table lists __this as its FIRST isParam entry, and
//paramTypeDescs/defaultValues are per-formal (index 0 == first visible
//parameter). The stub therefore creates formals = paramCount - 1 params,
//named from the locals entries after __this, with defaults/descriptors
//aligned from 0 — the same shapes the parser emits for a hand-written
//`C(int x)` member. `access` is the member's declared access (v2.7 wire).
//A record that cannot name a __this slot is a corrupt image: refusing it
//beats creating a stub whose call frame is one slot off (the
//walker/codegen claim split would trip the observedPeakCursor tripwire
//far from the cause).
//`producerIdx` (0.10.14): the record's index in the producer's own
//function table, clamped by the caller through ProducerIdxClamp — it
//rides the stub to the cross-unit slot record. No default value: every
//creation site states it explicitly.
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
	auto stubFormalNames = StubFormalNamesOf(cf, formals, true);

	//Return type: constructor records are RTK_Void → no return type
	//(HasReturn() == false, the D1 constructor shape).
	SnFieldExpr *pRetType = nullptr;
	if (cf.returnTypeKind != RTK_Void)
		pRetType = SynthTypeExprFromDesc(cf.returnTypeDesc, cm, loc,
			typeParamNames);

	auto *pParams = BuildStubFormalParamNodes(cf, cm, stubFormalNames,
		loc, typeParamNames);

	//Leaf name (phase 5 key vs. stub-name split): the resolver binds the
	//member by the caller's bare callee name.
	auto pName = new std::string(LeafNameOfKey(cf.name));
	auto *pFunc = new SnFunction(access, NF_Data, pRetType, pName,
		pParams, loc);
	ApplyStubFunctionFlags(pFunc, cf, producerIdx, nullptr);
	return pFunc;
}

} //namespace nlang
