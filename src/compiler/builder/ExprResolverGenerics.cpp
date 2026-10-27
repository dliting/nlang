/*--- ExprResolverGenerics.cpp — 内建泛型类实例化机器（List/Dict/func 合成声明缓存）。
    从 ExprResolverTypes.cpp 抽取（合并期可维护性重构，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnData.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace nlang
{


//Phase 8e-3: Built-in generic class instantiation cache.
//Key: (base class name, resolved type-arg SnField* pointers).
//Value: synthetic SnClassDecl representing this instantiation. The same
//key MUST always return the same SnClassDecl* — pointer identity is
//load-bearing for CalcTypeDistance and for VmBackend's class index lookup.
//
//At runtime, all instantiations share a single CompiledClass named "List"
//via VmBackend's name-prefix strip. The synthetic SnClassDecl is a
//compile-time artifact only.
struct GenericInstKey {
	std::string baseName;
	std::vector<SnField*> typeArgs;
	//Phase 13: out markers live on the type-arg expression nodes (NF_Out),
	//not on the canonical fields, so the key needs a parallel flag vector —
	//without it func<void,int> and func<void,out int> collapse into one
	//declaration and the first instantiation silently wins.
	//0.7.3 B: array-ness needs no flag vector anymore — an array-typed
	//argument IS the interned SnArrayTypeToken in typeArgs, so pointer
	//identity keeps List<int> and List<int[]> distinct keys.
	std::vector<uint8> outFlags;
	bool operator<(const GenericInstKey& rhs) const {
		if (baseName != rhs.baseName) return baseName < rhs.baseName;
		if (typeArgs.size() != rhs.typeArgs.size())
			return typeArgs.size() < rhs.typeArgs.size();
		if (outFlags != rhs.outFlags) return outFlags < rhs.outFlags;
		for (size_t i = 0; i < typeArgs.size(); ++i) {
			if (typeArgs[i] != rhs.typeArgs[i])
				return typeArgs[i] < rhs.typeArgs[i];
		}
		return false;
	}
};
static std::map<GenericInstKey, SnClassDecl*> s_genericInstances;

//Side table: synthetic class → its type arguments. Used by Access(SnMemberExpr&)
//to compute method return types (e.g., List<int>.Get() returns int = typeArgs[0]).
//Also mirrored on SnClassDecl::GenericTypeArgs() for VmBackend codegen.
static std::map<SnClassDecl*, std::vector<SnField*>> s_genericTypeArgs;

//Phase 13: parallel out-flag side table (same key discipline as
//s_genericTypeArgs). func's delegate-binding channel reads the signature
//(return type + out-marked params) from these two tables.
static std::map<SnClassDecl*, std::vector<uint8>> s_genericOutFlags;

//Phase 13: out-flag side-table accessor (same key discipline as
//GetGenericTypeArgs; map operator[] default-inserts on miss, matching
//the historical direct map access at the func bind sites).
const std::vector<uint8>& GetGenericOutFlags(SnClassDecl* pClass)
{
	return s_genericOutFlags[pClass];
}

//Lookup type arguments for a synthetic generic class. Returns empty vector
//if not a generic instantiation.
std::vector<SnField*> GetGenericTypeArgs(SnClassDecl* pClass)
{
	if (pClass && pClass->IsGenericInstantiation())
		return pClass->GenericTypeArgs();
	auto it = s_genericTypeArgs.find(pClass);
	if (it != s_genericTypeArgs.end())
		return it->second;
	return {};
}

//Returns true if class decl is a synthetic generic instantiation
//(e.g., List<int>). Used to dispatch member calls in Access(SnMemberExpr&).
bool IsGenericClassDecl(SnClassDecl* pClass)
{
	return pClass && pClass->IsGenericInstantiation();
}

//Mints (or fetches) a synthetic SnClassDecl for the given generic
//instantiation. Phase 8e-3: List<T> (arity 1). Phase 8e-4: Dict<K,V> (arity 2).
//Phase 13: func<R, P...> (variadic, >= 1). outFlags are normalized here
//so callers that infer instantiations (List element inference) can pass
//an empty vector while declaration-path keys stay distinct.
SnClassDecl* GetGenericClassDecl(const std::string& baseName,
	const std::vector<SnField*>& typeArgs, const std::vector<uint8>& outFlags,
	const ISourceLocation* pLoc)
{
	std::vector<uint8> normOutFlags(outFlags);
	normOutFlags.resize(typeArgs.size(), 0);
	GenericInstKey key{baseName, typeArgs, normOutFlags};
	auto it = s_genericInstances.find(key);
	if (it != s_genericInstances.end())
		return it->second;

	//Built-in generic + arity check. List/Dict have fixed arity; func is
	//variadic: first argument is the return type, the rest are params.
	bool isFunc = baseName == kBuiltinFuncTypeName;
	size_t expectedArity = (baseName == kBuiltinDictTypeName) ? 2 : 1;
	size_t minArity = isFunc ? 1 : expectedArity;
	size_t maxArity = isFunc ? static_cast<size_t>(-1) : expectedArity;
	if (!IsBuiltinGenericTypeName(baseName)
		|| typeArgs.size() < minArity || typeArgs.size() > maxArity)
		return nullptr;

	//Build display name e.g. "List<int>", "Dict<string, int>". Array-typed
	//arguments ARE interned tokens; their Name() is empty by design, so
	//render them via ToString ("Int32[]") — without this the split
	//instantiations would be indistinguishable in diagnostics.
	std::string instName = baseName + "<";
	for (size_t i = 0; i < typeArgs.size(); ++i) {
		if (i) instName += ", ";
		if (typeArgs[i]->Kind() == NK_ArrayTypeToken)
			instName += typeArgs[i]->ToString();
		else
			instName += typeArgs[i]->Name();
	}
	instName += ">";

	auto* pName = new std::string(instName);
	auto* pMembers = new PtrList<SnField>();
	ScriptLocation fallback;
	auto* pClass = new SnClassDecl(pName, nullptr, pMembers,
		DeclLocation(pLoc, fallback));
	pClass->SetBuiltinClass();
	pClass->SetGenericInstantiation();
	pClass->SetGenericTypeArgs(typeArgs);
	pClass->SetBaseName(baseName);
	s_genericInstances[key] = pClass;
	//Side table for member-call return-type lookup (List<int>.Get() → int).
	s_genericTypeArgs[pClass] = typeArgs;
	s_genericOutFlags[pClass] = normOutFlags;
	return pClass;
}


//==================== declared-generic machinery ====================
//
//`class Name<T, ...>` declarations (signature surfaces only) resolve
//their member signatures against interned placeholder types, and every
//`Name<Arg...>` use mints a synthetic instantiation whose members are
//signature deep-copies with placeholders substituted. Everything here
//is a compile-time artifact; the VM side reads the registries through
//GenericInstanceRegistry.h.

namespace {

struct TypeParamKey {
	const SnClassDecl* pTemplate;
	std::string name;
	bool operator<(const TypeParamKey& rhs) const {
		if (pTemplate != rhs.pTemplate) return pTemplate < rhs.pTemplate;
		return name < rhs.name;
	}
};
//Placeholder for one (template, type parameter) pair, interned on first
//request. The placeholder is an opaque class-typed node so ordinary type
//binding (IsTypeOrNamespaceBinding, EvalDataType) accepts it.
static std::map<TypeParamKey, SnField*> s_typeParamPlaceholders;
//Placeholder → its parameter position (the index into the template's
//TypeParamNames()); the substitution reads the actual argument here.
static std::map<const SnField*, size_t> s_placeholderIndex;

struct DeclaredInstKey {
	const SnClassDecl* pTemplate;
	std::vector<const SnField*> typeArgs;
	bool operator<(const DeclaredInstKey& rhs) const {
		if (pTemplate != rhs.pTemplate) return pTemplate < rhs.pTemplate;
		if (typeArgs.size() != rhs.typeArgs.size())
			return typeArgs.size() < rhs.typeArgs.size();
		for (size_t i = 0; i < typeArgs.size(); ++i)
			if (typeArgs[i] != rhs.typeArgs[i])
				return typeArgs[i] < rhs.typeArgs[i];
		return false;
	}
};
static std::map<DeclaredInstKey, SnClassDecl*> s_declaredInstances;
//Instantiation → its template (the ctor-call arm matches the published
//target's instantiation against the probed template through this).
static std::map<const SnClassDecl*, const SnClassDecl*> s_instanceTemplate;
//Instantiation → the unit whose resolution minted it (registration and
//method-table population read it per unit image).
static std::map<const SnClassDecl*, uint32_t> s_instanceOwner;
//Per-unit mint order for the registration walks.
static std::map<uint32_t, std::vector<SnClassDecl*>> s_instancesByUnit;

} //namespace

SnField* FindTypeParamPlaceholder(SnClassDecl& tmpl, const std::string& name,
	size_t position)
{
	const TypeParamKey key{&tmpl, name};
	auto it = s_typeParamPlaceholders.find(key);
	if (it != s_typeParamPlaceholders.end())
		return it->second;
	//An opaque class-typed node: every type-binding site accepts it and
	//nothing dereferences into it (members never reference placeholders).
	auto* pName = new std::string(name);
	auto* pPlaceholder = new SnClassDecl(pName, nullptr,
		new PtrList<SnField>(), *tmpl.Location());
	s_typeParamPlaceholders[key] = pPlaceholder;
	s_placeholderIndex[pPlaceholder] = position;
	return pPlaceholder;
}

bool IsTypeParamPlaceholder(const SnField* pField)
{
	return pField && s_placeholderIndex.count(pField) > 0;
}

size_t TypeParamIndexOf(const SnField* pPlaceholder)
{
	auto it = s_placeholderIndex.find(pPlaceholder);
	assert(it != s_placeholderIndex.end());
	return it->second;
}

SnClassDecl* DeclaredInstanceOf(const SnClassDecl* pInstance)
{
	auto it = s_instanceTemplate.find(pInstance);
	return it != s_instanceTemplate.end()
		? const_cast<SnClassDecl*>(it->second) : nullptr;
}

void ForEachDeclaredGenericInstance(uint32_t ownerUnit,
	const std::function<void(SnClassDecl&)>& visitor)
{
	auto it = s_instancesByUnit.find(ownerUnit);
	if (it == s_instancesByUnit.end())
		return;
	for (auto* pInstance : it->second)
		visitor(*pInstance);
}

//One template member's signature deep-copy: native members carry no
//bodies, so the copy is the metadata surface only — return type, formal
//parameters (no default values; the declaration checker rejects those).
SnField* CloneGenericMember(SnField& src,
	const std::vector<SnField*>& typeArgs,
	const std::function<SnFieldExpr*(SnFieldExpr&)>& substitute)
{
	if (src.Kind() == NK_Function)
	{
		auto& fn = static_cast<SnFunction&>(src);
		SnFieldExpr* pRetCopy = fn.ReturnType()
			? substitute(*fn.ReturnType()) : nullptr;
		auto* pParams = new PtrList<SnFormalParam>();
		for (auto& param : fn.Params())
		{
			auto* pTypeCopy = param.Type()
				? substitute(*param.Type()) : nullptr;
			pParams->push_back(new SnFormalParam(param.Flags(), pTypeCopy,
				new std::string(param.Name()), nullptr, *param.Location()));
		}
		return new SnFunction(fn.AccessType(), fn.Flags(), pRetCopy,
			new std::string(fn.Name()), pParams, *fn.Location());
	}
	if (src.Kind() == NK_ClassField)
	{
		auto& field = static_cast<SnClassField&>(src);
		auto* pTypeCopy = field.Type()
			? substitute(*field.Type()) : nullptr;
		return new SnClassField(pTypeCopy, new std::string(field.Name()),
			field.AccessType(), *field.Location());
	}
	return nullptr;
}

//GetDeclaredGenericClassDecl's member-copy loop: each member copy
//substitutes its type expressions before joining the instance. The
//copies accumulate BEFORE the SnClassDecl ctor runs — the ctor adopts
//the list through a by-value UniquePtrList and deletes the shell, so
//the returned list pointer must not be touched again after adoption.
PtrList<SnField>* ExprResolveAccessor::CloneInstanceMembers(
	SnClassDecl& tmpl, const std::vector<SnField*>& typeArgs)
{
	auto* pMembers = new PtrList<SnField>();
	for (auto& member : tmpl.Members())
	{
		auto* pCopy = CloneGenericMember(member, typeArgs,
			[this, &typeArgs](SnFieldExpr& srcType)
			{ return SubstituteTypeExpr(srcType, typeArgs); });
		if (!pCopy)
		{
			//The ctor has not adopted the copies yet — they are plain
			//raw news here; release them before bailing out.
			for (auto* pPartial : *pMembers)
				delete pPartial;
			delete pMembers;
			m_Env.Log(CLL_Error, member.Location(),
				"The class \"%s\" is a generic declaration: the member "
				"\"%s\" has a shape instantiations cannot carry.",
				tmpl.Name().c_str(), member.Name().c_str());
			return nullptr;
		}
		pMembers->push_back(pCopy);
	}
	return pMembers;
}

SnClassDecl* ExprResolveAccessor::GetDeclaredGenericClassDecl(
	SnClassDecl& tmpl, const std::vector<SnField*>& typeArgs,
	uint32_t ownerUnit)
{
	DeclaredInstKey key{&tmpl, {}};
	key.typeArgs.reserve(typeArgs.size());
	for (auto* pArg : typeArgs)
		key.typeArgs.push_back(pArg);
	auto it = s_declaredInstances.find(key);
	if (it != s_declaredInstances.end())
		return it->second;

	//Erasure naming: the instantiation shares the template's bare name,
	//the same way every List<T> shares the backing "List" class — the
	//VM keys the CompiledClass, the intrinsic channel and the ctor
	//classification off the bare name, so no consumer needs a
	//display-name special case. Distinct instantiations stay distinct
	//compile-time types through node identity.

	//Signatures in, placeholders out (CloneInstanceMembers).
	auto* pMembers = CloneInstanceMembers(tmpl, typeArgs);
	if (!pMembers)
		return nullptr;

	auto* pName = new std::string(tmpl.Name());
	auto* pInstance = new SnClassDecl(pName, nullptr, pMembers,
		*tmpl.Location());
	pInstance->SuperClass(tmpl.SuperClass());
	pInstance->SetGenericInstantiation();
	pInstance->SetBaseName(tmpl.Name());
	pInstance->SetGenericTypeArgs(typeArgs);

	s_declaredInstances[key] = pInstance;
	s_instanceTemplate[pInstance] = &tmpl;
	s_instanceOwner[pInstance] = ownerUnit;
	s_instancesByUnit[ownerUnit].push_back(pInstance);
	return pInstance;
}

SnFieldExpr* ExprResolveAccessor::SubstituteTypeExpr(
	SnFieldExpr& srcType, const std::vector<SnField*>& typeArgs)
{
	//Name leaf: transfer the source binding; a placeholder leaf binds to
	//the actual argument instead. Unresolved leaves stay unresolved (the
	//template member's own diagnostic already fired).
	if (srcType.Kind() == NK_NameExpr)
	{
		auto& name = static_cast<SnNameExpr&>(srcType);
		auto* pInner = dynamic_cast<SnIdentifierExpr*>(name.Expr());
		if (!pInner)
			return nullptr;
		auto* pCopy = new SnNameExpr(
			new SnIdentifierExpr(new std::string(pInner->Name()),
				*srcType.Location()), *srcType.Location());
		if (name.IsResolved())
		{
			auto* pBinding = IsTypeParamPlaceholder(name.Field())
				? typeArgs[TypeParamIndexOf(name.Field())]
				: name.Field();
			ResolveFieldExprAs(*pCopy, pBinding);
		}
		return pCopy;
	}
	//Array: substitute the element, then re-intern the array token from
	//the substituted element (the source token encodes the placeholder).
	if (srcType.Kind() == NK_ArrayTypeExpr)
	{
		auto& arr = static_cast<SnArrayTypeExpr&>(srcType);
		auto* pElemCopy = SubstituteTypeExpr(*arr.ElementType(), typeArgs);
		if (!pElemCopy)
			return nullptr;
		auto* pCopy = new SnArrayTypeExpr(pElemCopy, *srcType.Location());
		if (arr.IsResolved())
		{
			pCopy->EvalDataType(pElemCopy->Field());
			pCopy->AddFlags(NF_Resolved);
			BindArrayTypeToken(*pCopy);
		}
		return pCopy;
	}
	//Nested instantiation (`Future<T>`, `List<T>`): substitute base and
	//arguments, then resolve the copy — the instantiation cache mints
	//the instantiation for the ACTUAL arguments.
	if (srcType.Kind() == NK_GenericTypeExpr)
		return SubstituteGenericTypeExpr(
			static_cast<SnGenericTypeExpr&>(srcType), typeArgs);
	return nullptr;
}

SnFieldExpr* ExprResolveAccessor::SubstituteGenericTypeExpr(
	SnGenericTypeExpr& gen, const std::vector<SnField*>& typeArgs)
{
	auto* pBaseCopy = SubstituteTypeExpr(*gen.Base(), typeArgs);
	if (!pBaseCopy)
		return nullptr;
	auto* pArgs = new std::vector<SnFieldExpr*>;
	for (auto* pArg : gen.TypeArgs())
	{
		auto* pArgCopy = SubstituteTypeExpr(*pArg, typeArgs);
		if (!pArgCopy)
			return nullptr;
		pArgs->push_back(pArgCopy);
	}
	auto* pCopy = new SnGenericTypeExpr(pBaseCopy, pArgs,
		*gen.Location());
	if (gen.IsResolved())
		Access(*pCopy);
	return pCopy;
}

ExprResolveAccessor::TypeParamBindResult
ExprResolveAccessor::BindTypeParamPlaceholderIfAny(SnIdentifierExpr& idExpr)
{
	const std::string& name = idExpr.Name();
	SyntaxNode* pMember = nullptr;
	for (SyntaxNode* p = idExpr.Parent(); p; p = p->Parent())
	{
		const auto kind = p->Kind();
		if (kind == NK_ClassDecl)
		{
			auto& cls = static_cast<SnClassDecl&>(*p);
			if (!cls.IsGenericDeclaration())
				return TypeParamBindResult::NotApplicable;
			const auto& params = cls.TypeParamNames();
			const auto paramIt = std::find(params.begin(), params.end(),
				name);
			if (paramIt == params.end())
				return TypeParamBindResult::NotApplicable;
			//Fields share the VM layout across instantiations; a generic
			//field type would give Box<int> and Box<string> different
			//layouts, so the reference is a declaration error.
			if (pMember && pMember->Kind() == NK_ClassField)
			{
				auto& field = static_cast<const SnField&>(*pMember);
				m_Env.Log(CLL_Error, idExpr.Location(),
					"The field \"%s\" of generic class \"%s\" cannot "
					"reference the type parameter \"%s\" (fields must "
					"keep the same layout in every instantiation).",
					field.Name().c_str(), cls.Name().c_str(),
					name.c_str());
				return TypeParamBindResult::Rejected;
			}
			auto* pPlaceholder = FindTypeParamPlaceholder(cls, name,
				static_cast<size_t>(paramIt - params.begin()));
			ResolveFieldExprAs(idExpr, pPlaceholder);
			return TypeParamBindResult::Bound;
		}
		if (!pMember && (kind == NK_Function || kind == NK_ClassField))
			pMember = p;
	}
	return TypeParamBindResult::NotApplicable;
}


} //namespace nlang
