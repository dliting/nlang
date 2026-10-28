/*--- ExprResolverGenericMint.cpp — 声明泛型的铸造机器：实例注册表实现、
    成员签名深拷贝（占位符替换）与铸造咽喉点 GetDeclaredGenericClassDecl。
    从 ExprResolverGenerics.cpp 抽取（source-size 守卫驱动的拆分，零行为变化）。
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

//==================== declared-generic machinery ====================
//
//`class Name<T, ...>` declarations (signature surfaces only) resolve
//their member signatures against interned placeholder types, and every
//`Name<Arg...>` use mints a synthetic instantiation whose members are
//signature deep-copies with placeholders substituted. Everything here
//is a compile-time artifact; the VM side reads the registry through
//GenericInstanceRegistry.h. The registry instance is owned by
//ModuleRegistry — one per BUILD (process-global maps would be keyed
//by per-build pointers and unit indices; see that header).

GenericInstanceRegistry::~GenericInstanceRegistry()
{
	//The registry owns every node it minted. Instances and placeholders
	//are detached from any tree, so nothing else frees them.
	for (auto& kv : m_instances)
		delete kv.second;
	for (auto& kv : m_placeholders)
		delete kv.second;
}

SnField* GenericInstanceRegistry::PlaceholderFor(SnClassDecl& tmpl,
	const std::string& name, size_t position)
{
	const PlaceholderKey key{&tmpl, name};
	auto it = m_placeholders.find(key);
	if (it != m_placeholders.end())
		return it->second;
	//An opaque class-typed node: every type-binding site accepts it and
	//nothing dereferences into it (members never reference placeholders).
	auto* pName = new std::string(name);
	auto* pPlaceholder = new SnClassDecl(pName, nullptr,
		new PtrList<SnField>(), *tmpl.Location());
	m_placeholders[key] = pPlaceholder;
	m_placeholderIndex[pPlaceholder] = position;
	return pPlaceholder;
}

bool GenericInstanceRegistry::IsPlaceholder(const SnField* pField) const
{
	return pField && m_placeholderIndex.count(pField) > 0;
}

size_t GenericInstanceRegistry::PlaceholderIndex(
	const SnField* pPlaceholder) const
{
	auto it = m_placeholderIndex.find(pPlaceholder);
	assert(it != m_placeholderIndex.end());
	return it->second;
}

bool GenericInstanceRegistry::HasPlaceholderArg(
	const SnClassDecl& inst) const
{
	for (const SnField* pArg : inst.GenericTypeArgs()) {
		if (IsPlaceholder(pArg))
			return true;
		if (pArg->Kind() == NK_ClassDecl) {
			const auto& argInst = static_cast<const SnClassDecl&>(*pArg);
			if (argInst.IsGenericInstantiation()
					&& HasPlaceholderArg(argInst))
				return true;
		}
	}
	return false;
}

SnClassDecl* GenericInstanceRegistry::InstanceOf(
	const SnClassDecl* pInstance) const
{
	auto it = m_instanceTemplate.find(pInstance);
	return it != m_instanceTemplate.end()
		? const_cast<SnClassDecl*>(it->second) : nullptr;
}

void GenericInstanceRegistry::AdoptInstance(SnClassDecl* pInstance,
	const SnClassDecl* pTemplate,
	const std::vector<const SnField*>& typeArgs, uint32_t ownerUnit)
{
	InstanceKey key{pTemplate, typeArgs};
	m_instances[key] = pInstance;
	m_instanceTemplate[pInstance] = pTemplate;
	m_instancesByUnit[ownerUnit].push_back(pInstance);
}

SnClassDecl* GenericInstanceRegistry::FindInstance(
	const SnClassDecl* pTemplate,
	const std::vector<const SnField*>& typeArgs) const
{
	auto it = m_instances.find(InstanceKey{pTemplate, typeArgs});
	return it != m_instances.end() ? it->second : nullptr;
}

void GenericInstanceRegistry::ForEachInstance(uint32_t ownerUnit,
	const std::function<void(SnClassDecl&)>& visitor) const
{
	auto it = m_instancesByUnit.find(ownerUnit);
	if (it == m_instancesByUnit.end())
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

//The mint itself: a detached class decl carrying the substituted
//member copies under erasure naming — the instantiation shares the
//template's bare name, the same way every List<T> shares the backing
//"List" class (the VM keys the CompiledClass, the intrinsic channel
//and the ctor classification off the bare name, so no consumer needs
//a display-name special case; distinct instantiations stay distinct
//compile-time types through node identity).
static SnClassDecl *MintInstanceOf(const SnClassDecl &tmpl,
	const std::vector<SnField*> &typeArgs, PtrList<SnField> *pMembers)
{
	auto* pInstance = new SnClassDecl(new std::string(tmpl.Name()),
		nullptr, pMembers, *tmpl.Location());
	pInstance->SuperClass(tmpl.SuperClass());
	pInstance->SetGenericInstantiation();
	pInstance->SetBaseName(tmpl.Name());
	pInstance->SetGenericTypeArgs(typeArgs);
	return pInstance;
}

//Visibility rule: a prelude (or ownerless) template's instantiation
//is visible in every unit image — prelude declarations register in
//every image and untagged instances resolve through ClassSlotFor's
//own arm, so the erased row must exist wherever a reference can
//land. The mint context's unit is timing-dependent (whichever unit
//resolves a shape first would claim it), so the bucket cannot carry
//this visibility: file such instances ownerless — the NO_OWNER
//bucket is visited by every per-unit walk. A user-unit template's
//instances keep the resolving unit's bucket, where the use sites'
//image lives.
static uint32_t InstanceOwnerUnit(const ModuleRegistry &registry,
	const SnClassDecl &tmpl, uint32_t ownerUnit)
{
	const uint32_t tmplOwner = registry.OwnerOf(tmpl);
	if (tmplOwner == ModuleRegistry::NO_OWNER
			|| registry.IsPreludeModule(tmplOwner))
		return ModuleRegistry::NO_OWNER;
	return ownerUnit;
}

SnClassDecl* ExprResolveAccessor::GetDeclaredGenericClassDecl(
	SnClassDecl& tmpl, const std::vector<SnField*>& typeArgs,
	uint32_t ownerUnit)
{
	auto& generics = m_Env.Registry().DeclaredGenerics();
	std::vector<const SnField*> keyArgs(typeArgs.begin(), typeArgs.end());
	if (auto* pCached = generics.FindInstance(&tmpl, keyArgs))
		return pCached;

	//Invariant: clone SIGNATURE-COMPLETE templates only. The mint can
	//fire from the declaration pass (a class field of declared-generic
	//type) before the template's own signatures resolved — cloning then
	//copies unresolved type expressions and the cache would serve a
	//broken instance to every later phase. Completing the template HERE,
	//at the single chokepoint every consumer reaches the mint through,
	//makes instantiation order-independent (the same machinery the
	//ordinary pre-pass runs; per-member flags make the later pass a
	//no-op). ResolveDataTypes is idempotent and flag-guarded.
	ExprResolver templateCompleter(m_Env);
	templateCompleter.ResolveDataTypes(tmpl, tmpl);
	//Implicit-root invariant, order-independent: the merged root walks
	//user code ahead of the prelude, so the mint can fire before the
	//template's own class pass applies the implicit Object chain — a
	//cached instance would keep the null super forever and every
	//inheritance-distance check (an equals(Object) argument binding)
	//would reject it. Complete the link at the same chokepoint as the
	//signatures, mirroring the visibility rule below.
	EnsureImplicitObjectSuperclass(tmpl, m_Env.Registry());

	//Signatures in, placeholders out (CloneInstanceMembers).
	auto* pMembers = CloneInstanceMembers(tmpl, typeArgs);
	if (!pMembers)
		return nullptr;

	auto* pInstance = MintInstanceOf(tmpl, typeArgs, pMembers);
	generics.AdoptInstance(pInstance, &tmpl, keyArgs,
		InstanceOwnerUnit(m_Env.Registry(), tmpl, ownerUnit));
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
			auto& generics = m_Env.Registry().DeclaredGenerics();
			auto* pBinding = generics.IsPlaceholder(name.Field())
				? typeArgs[generics.PlaceholderIndex(name.Field())]
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
			auto* pPlaceholder = m_Env.Registry().DeclaredGenerics()
				.PlaceholderFor(cls, name,
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
