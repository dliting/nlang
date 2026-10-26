#pragma once
/*---
    CompiledModuleStubMembers.hpp — 类型桩（class/struct/interface）与自由
    函数桩的 AST 重建（0.9.12 D3）。从 CompiledModuleNodeBuilder.hpp 拆出
    （源尺寸守卫）；0.9.11 的构造器桩助手住在 CompiledModuleCtorStub.hpp，
    类型/默认值合成助手住在同族支持头文件里。自由函数——不触碰 builder 状态。
---*/
#include "CompiledModuleCtorStub.hpp"
#include "CompiledModuleDefaultSupport.hpp"
#include "CompiledModuleStubSupport.hpp"
#include "ModuleRegistry.h"
#include "SnMisc.h"
#include "SnData.h"
#include "SnExpressions.h"
#include "SyntaxTree.h"
#include "nlang/vm/CompiledModule.h"
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nlang
{

//Build a SnFunction stub for an imported free-function or class/interface
//static record. The stub has no Body() — VmBackend.RegisterFunctions and
//GenerateAllBytecode already skip bodyless functions, and the NF_Imported
//flag adds an explicit IsImported() gate for safety. `access` is the
//member's declared access (0.9.12: class statics carry it on the wire);
//free functions and interface statics mint FA_Public at the call site.
inline SnFunction *CreateFunctionStub(const CompiledFunction &cf,
	const CompiledModule &cm, const ISourceLocation &loc,
	FieldAccessType access)
{
	//Return type. RTK_Void → no return type (SnFunction::HasReturn() == false).
	//v1.12: the serialized descriptor supplies the true type; the
	//NonSerialized sentinel (func/interface returns) degrades to the
	//int32 placeholder.
	SnFieldExpr *pRetType = nullptr;
	if (cf.returnTypeKind != RTK_Void)
		pRetType = SynthTypeExprFromDesc(cf.returnTypeDesc, cm, loc);

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
	//0.9.10 D5: the static flag rides the record — the resolver's
	//receiver-namespace split (MatchesReceiverForm) and the emitter's
	//slot arity (FunctionSlotFor: statics take no this slot) both key
	//on it.
	if (cf.isStatic)
		pFunc->AddFlags(NF_Static);

	return pFunc;
}

//Build a SnStructDecl stub for an imported CompiledStruct.
//Fields stay wire-only — VmBackend.RegisterStructs skips imported
//stubs entirely (R3 + IsImported gating) and external struct FIELD
//access is out of scope. 0.9.11 v2.6: the CONSTRUCTORS are rebuilt
//(from constructorIndices, declaration order) — a cross-module
//`pkg.Vec(3,4)` needs member declarations to bind against; the call
//itself emits an imported function slot, never local bytecode.
//0.9.12 (v2.7): each constructor stub carries its declared access
//(constructorAccess) — a private struct ctor stays private at the
//consumer.
inline SnStructDecl *CreateStructStub(const CompiledStruct &cs,
	const CompiledModule &cm, const ISourceLocation &loc)
{
	//Field info stays in the loaded CompiledModule, which rides along
	//to the load-time linker as a peer image.
	//SnStructDecl takes UniquePtrList<SnField> by value; pass a raw
	//PtrList* whose ownership is transferred via the implicit
	//inner_collection* constructor.
	auto *pMembers = new PtrList<SnField>();
	for (size_t ci = 0; ci < cs.constructorIndices.size(); ++ci)
	{
		//Out-of-range is a corrupt image — the loader validated the
		//counts, the bound check keeps stub minting defensive.
		const uint16_t idx = cs.constructorIndices[ci];
		if (idx >= cm.functions.size())
			continue;
		const FieldAccessType access = ci < cs.constructorAccess.size()
			? static_cast<FieldAccessType>(cs.constructorAccess[ci])
			: FA_Public;
		pMembers->push_back(
			CreateMethodStub(cm.functions[idx], cm, loc, access));
	}
	auto pName = new std::string(LeafNameOfKey(cs.name));
	auto *pDecl = new SnStructDecl(pName, pMembers, loc);
	pDecl->AddFlags(NF_Imported);   //R6-2
	return pDecl;
}

//Build a SnClassDecl stub for an imported CompiledClass. 0.9.12 (v2.7):
//the stub carries the class's full member surface — own fields (private
//included: the consumer's offset walk counts every slot along the stub
//chain), methods (static and instance, each with its declared access and
//virtual flag), and constructors with their declared access. Every
//member is a bodyless NF_Imported stub: calls emit imported function
//slots, never local bytecode. Inherited fields and methods come through
//the superclass stub chain (LinkSuperclassStub), not the record.
inline SnClassDecl *CreateClassStub(const CompiledClass &cc,
	const CompiledModule &cm, const ISourceLocation &loc)
{
	auto pName = new std::string(LeafNameOfKey(cc.name));
	auto *pMembers = new PtrList<SnField>();

	//Own fields only (the wire carries own-only since v2.7). Private
	//fields are rebuilt too — FindClassFieldOffset counts every slot
	//along the chain, and the visibility gate rejects non-public reads
	//later, at the use site. The v1.12 descriptor is the primary type
	//source; the legacy parallel kind array is the fallback (scalars
	//resolve exactly through SynthTypeExprFromDesc's RuntimeTypeKind
	//arm; anything else degrades to the int32 placeholder).
	for (uint16_t fi = 0; fi < cc.fieldCount
		&& fi < cc.fieldNames.size(); ++fi)
	{
		TypeDesc td;
		if (fi < cc.fieldTypeDescs.size())
			td = cc.fieldTypeDescs[fi];
		if (td.kind == RTK_NonSerialized && fi < cc.fieldTypeKinds.size())
			td.kind = cc.fieldTypeKinds[fi];
		auto *pField = new SnClassField(
			SynthTypeExprFromDesc(td, cm, loc),
			new std::string(cc.fieldNames[fi]),
			fi < cc.fieldAccess.size()
				? static_cast<FieldAccessType>(cc.fieldAccess[fi])
				: FA_Public,
			loc);
		pMembers->push_back(pField);
	}

	//Methods. The producer registers every member function once and
	//pushes constructors into methodIndices as well
	//(PopulateClassMethods); constructors are rebuilt from
	//constructorIndices below, so skip their entries here — otherwise
	//every constructor would also appear as a ghost instance method.
	std::unordered_set<uint16_t> ctorSet(
		cc.constructorIndices.begin(), cc.constructorIndices.end());
	for (size_t mi = 0; mi < cc.methodIndices.size(); ++mi)
	{
		const uint16_t idx = cc.methodIndices[mi];
		//Ctor entry / defensive bound (the loader validated the indices).
		if (ctorSet.count(idx) || idx >= cm.functions.size())
			continue;
		const FieldAccessType access = mi < cc.methodAccess.size()
			? static_cast<FieldAccessType>(cc.methodAccess[mi])
			: FA_Public;
		//Staticness rides the function record (isStatic); the access
		//byte is orthogonal.
		SnFunction *pMethod = cm.functions[idx].isStatic
			? CreateFunctionStub(cm.functions[idx], cm, loc, access)
			: CreateMethodStub(cm.functions[idx], cm, loc, access);
		if (mi < cc.methodVirtual.size() && cc.methodVirtual[mi])
			pMethod->AddFlags(NF_Virtual);
		pMembers->push_back(pMethod);
	}

	//Constructors carry their declared access (v2.7) — a private ctor
	//stays private at the consumer (`new lib.C()` / `lib.C(...)` are
	//rejected there).
	for (size_t ci = 0; ci < cc.constructorIndices.size(); ++ci)
	{
		const uint16_t idx = cc.constructorIndices[ci];
		if (idx >= cm.functions.size())
			continue;
		const FieldAccessType access = ci < cc.constructorAccess.size()
			? static_cast<FieldAccessType>(cc.constructorAccess[ci])
			: FA_Public;
		pMembers->push_back(
			CreateMethodStub(cm.functions[idx], cm, loc, access));
	}

	auto *pDecl = new SnClassDecl(pName, nullptr, pMembers, loc);
	pDecl->AddFlags(NF_Imported);   //R6-2
	return pDecl;
}

//0.9.10 D5 + 0.9.12 (v2.7): build a SnInterfaceDecl stub — statics and
//constants as before, plus bodyless instance-method stubs rebuilt from
//the signature records. Interface-typed calls dispatch by name through
//the receiver's runtime class (EmitMemberInterfaceCall), so the stubs
//need no NF_Virtual and no bodies; the implements contract checks bind
//against these signatures across modules.
inline SnInterfaceDecl *CreateInterfaceStub(const CompiledInterface &ci,
	const CompiledModule &cm, const ISourceLocation &loc)
{
	auto pName = new std::string(LeafNameOfKey(ci.name));
	auto *pMembers = new PtrList<SnField>();
	for (auto idx : ci.staticMethodIndices)
	{
		if (idx < cm.functions.size())
			pMembers->push_back(
				CreateFunctionStub(cm.functions[idx], cm, loc, FA_Public));
	}
	//0.9.10 D3: exported constants rebuild as `public static const`
	//fields whose synthesized literal initializer folds again at the
	//consumer's use sites (the declaration contract already ran at
	//the producer; imported stubs are exempt from re-checking).
	for (const auto &constant : ci.constants)
	{
		SnExpression *pInit = SynthScalarDefault(constant.value, cm, loc);
		if (!pInit)
			throw std::runtime_error(
				"imported interface '" + ci.name
				+ "' has a malformed constant value record");
		auto *pField = new SnClassField(
			SynthTypeExprFromDesc(constant.type, cm, loc),
			new std::string(constant.name), FA_Public, loc, pInit);
		pField->AddFlags(NF_Static);
		pField->AddFlags(NF_Const);
		pMembers->push_back(pField);
	}
	//0.9.12 (v2.7): instance-method signatures. The signature records
	//carry no formal names (unlike function records' locals tables), so
	//the stubs mint "p<i>" placeholders — positional binding works,
	//named-argument calls against an imported interface method do not
	//bind. Interface members are implicitly public and non-static.
	for (const auto &sig : ci.instanceMethods)
	{
		std::vector<std::string> stubFormalNames(sig.paramTypeDescs.size());
		for (size_t i = 0; i < sig.paramTypeDescs.size(); ++i)
			stubFormalNames[i] = "p" + std::to_string(i);
		auto *pParams = new PtrList<SnFormalParam>();
		for (size_t i = 0; i < sig.paramTypeDescs.size(); ++i)
		{
			const ParamTypeDesc &ptd = sig.paramTypeDescs[i];
			NodeBits paramFlags = NF_NONE;
			if (ptd.flags & PTDF_Out)
				paramFlags |= NF_Out;
			SnExpression *pDefault = nullptr;
			if (i < sig.defaultValues.size())
				pDefault = SynthDefaultExpr(sig.defaultValues[i], cm,
					sig.name, static_cast<uint16_t>(i), stubFormalNames, loc);
			pParams->push_back(new SnFormalParam(paramFlags,
				SynthTypeExprFromDesc(ptd.type, cm, loc),
				new std::string(stubFormalNames[i]), pDefault, loc));
		}
		//Void signatures carry the RTK_Void kind byte (the producer
		//stamps it); any other kind synthesizes through the standard
		//descriptor channel (NonSerialized degrades to the int32
		//placeholder, mirroring the function-record degradation).
		SnFieldExpr *pRetType = nullptr;
		if (sig.returnType.kind != RTK_Void)
			pRetType = SynthTypeExprFromDesc(sig.returnType, cm, loc);
		auto *pFunc = new SnFunction(FA_Public, NF_Data, pRetType,
			new std::string(sig.name), pParams, loc);
		pFunc->AddFlags(NF_Imported);   //R6-2
		pMembers->push_back(pFunc);
	}
	auto *pDecl = new SnInterfaceDecl(pName, pMembers, loc);
	pDecl->AddFlags(NF_Imported);   //R6-2
	return pDecl;
}

//0.9.10 D5: point a class stub's SuperClass at its peer stub from the
//same module so inherited members resolve through the normal chain
//walk. A super from another imported module, a synthesized builtin, or
//a DECLARED prelude class (owner-gated — e.g. the Exception base of an
//imported exception subclass) falls back to the consumer root's entry;
//a consumer-owned class of the same name is NOT hijacked. registry may
//be null in hosts without one (the prelude arm is skipped).
inline void LinkSuperclassStub(SnClassDecl &stub, const CompiledClass &cc,
	const CompiledModule &cm,
	const std::unordered_map<std::string, SnClassDecl*> &peerStubs,
	SnNamespace &root, const ModuleRegistry *registry)
{
	if (cc.superClassIdx < 0
		|| cc.superClassIdx >= static_cast<int16_t>(cm.classes.size()))
		return;
	const std::string superLeaf =
		LeafNameOfKey(cm.classes[cc.superClassIdx].name);
	auto it = peerStubs.find(superLeaf);
	if (it != peerStubs.end())
	{
		stub.SuperClass(it->second);
		return;
	}
	SnField *pRoot = root.FindField(superLeaf);
	if (pRoot && pRoot->Kind() == NK_ClassDecl)
	{
		auto *pRootClass = static_cast<SnClassDecl*>(pRoot);
		if (pRootClass->ContainFlags(NF_Imported)
			|| pRootClass->IsBuiltinClass()
			|| (registry
				&& registry->IsPreludeModule(
					registry->OwnerOf(*pRootClass))))
			stub.SuperClass(pRootClass);
	}
}

} //namespace nlang
